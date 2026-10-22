#include "multicam-sync-dock.h"
#include "multicam-status.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QDateTime>
#include <QColor>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLabel>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QSettings>
#include <QSet>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <algorithm>
#include <memory>
#include <thread>

namespace {
constexpr char kDockId[] = "sei-stamper.multicam-sync-center";
constexpr uint64_t kAlignmentToleranceNs = 16666667ULL;
QWidget *dockWidget;
QTableWidget *tableWidget;
QLabel *summaryLabel;
QCheckBox *sceneDelayEnabled;
QSpinBox *sceneDelayMs;
QListWidget *sceneDelayScenes;
QTimer *sceneDelayTimer;
obs_source_t *lastProgramScene;
obs_source_t *pendingProgramScene;
QSet<QString> selectedSceneNames;
bool loadingSceneSettings;

QString scene_settings_path()
{
  char *path = obs_module_config_path("multicam-scene-delay.ini");
  if (!path) return QString();
  QString result = QString::fromUtf8(path);
  bfree(path);
  return result;
}

void save_scene_settings()
{
  if (loadingSceneSettings || !sceneDelayEnabled || !sceneDelayMs) return;
  QString path = scene_settings_path();
  if (path.isEmpty()) return;
  QDir().mkpath(QFileInfo(path).absolutePath());
  QSettings settings(path, QSettings::IniFormat);
  settings.setValue(QStringLiteral("enabled"), sceneDelayEnabled->isChecked());
  settings.setValue(QStringLiteral("delay_ms"), sceneDelayMs->value());
  QStringList names;
  for (const QString &name : selectedSceneNames) names.push_back(name);
  settings.setValue(QStringLiteral("selected_scenes"), names);
  settings.sync();
}

void refresh_scene_list()
{
  if (!sceneDelayScenes) return;
  loadingSceneSettings = true;
  sceneDelayScenes->clear();
  obs_frontend_source_list scenes = {};
  obs_frontend_get_scenes(&scenes);
  for (size_t i = 0; i < scenes.sources.num; ++i) {
    obs_source_t *source = scenes.sources.array[i];
    const QString name = QString::fromUtf8(obs_source_get_name(source));
    auto *item = new QListWidgetItem(name, sceneDelayScenes);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(selectedSceneNames.contains(name) ? Qt::Checked : Qt::Unchecked);
  }
  obs_frontend_source_list_free(&scenes);
  loadingSceneSettings = false;
}

void set_last_program_scene(obs_source_t *scene)
{
  obs_source_t *replacement = scene ? obs_source_get_ref(scene) : nullptr;
  if (lastProgramScene) obs_source_release(lastProgramScene);
  lastProgramScene = replacement;
}

bool set_scene_from_worker(obs_source_t *scene)
{
  obs_source_t *sceneRef = scene ? obs_source_get_ref(scene) : nullptr;
  if (!sceneRef) return false;
  try {
    std::thread([sceneRef] {
      /* OBS 32.2.2's public setter waits for the frontend UI thread. Calling it
       * from this worker avoids blocking/deadlocking the UI event callback. */
      obs_frontend_set_current_scene(sceneRef);
      obs_source_release(sceneRef);
    }).detach();
  } catch (...) {
    obs_source_release(sceneRef);
    return false;
  }
  return true;
}

void cancel_pending_scene()
{
  if (sceneDelayTimer) sceneDelayTimer->stop();
  if (pendingProgramScene) obs_source_release(pendingProgramScene);
  pendingProgramScene = nullptr;
}

void apply_pending_scene()
{
  if (!pendingProgramScene) return;
  obs_source_t *target = pendingProgramScene;
  pendingProgramScene = nullptr;
  if (lastProgramScene) obs_source_release(lastProgramScene);
  if (set_scene_from_worker(target)) {
    if (lastProgramScene) obs_source_release(lastProgramScene);
    lastProgramScene = obs_source_get_ref(target);
  }
  obs_source_release(target);
}

void handle_program_scene_changed(obs_source_t *current)
{
  if (!current || !dockWidget) return;
  if (!lastProgramScene) {
    set_last_program_scene(current);
    return;
  }
  if (current == lastProgramScene) return;

  const QString currentName = QString::fromUtf8(obs_source_get_name(current));
  const bool defer = sceneDelayEnabled && sceneDelayEnabled->isChecked() &&
                     sceneDelayMs && sceneDelayMs->value() > 0 &&
                     selectedSceneNames.contains(currentName);
  if (!defer) {
    cancel_pending_scene();
    set_last_program_scene(current);
    return;
  }

  cancel_pending_scene();
  pendingProgramScene = obs_source_get_ref(current);
  if (!pendingProgramScene) return;
  /* OBS exposes scene-changed after the requested switch. Restore the previous
   * program scene immediately, then commit the selected target when the timer
   * expires. The feature is opt-in and applies only to checked scene names. */
  if (!set_scene_from_worker(lastProgramScene)) {
    cancel_pending_scene();
    return;
  }
  sceneDelayTimer->start(sceneDelayMs->value());
}

void frontend_event(enum obs_frontend_event event, void *opaque)
{
  UNUSED_PARAMETER(opaque);
  QWidget *target = dockWidget;
  if (!target) return;
  if (event == OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED ||
      event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED) {
    QMetaObject::invokeMethod(target, [] { refresh_scene_list(); },
                              Qt::QueuedConnection);
  } else if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING ||
             event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP) {
    QMetaObject::invokeMethod(target, [] {
      cancel_pending_scene();
      if (lastProgramScene) obs_source_release(lastProgramScene);
      lastProgramScene = nullptr;
    }, Qt::QueuedConnection);
  } else if (event == OBS_FRONTEND_EVENT_SCENE_CHANGED) {
    obs_source_t *current = obs_frontend_get_current_scene();
    if (!current) return;
    auto currentRef = std::shared_ptr<obs_source_t>(current, obs_source_release);
    QMetaObject::invokeMethod(target, [currentRef] {
      handle_program_scene_changed(currentRef.get());
    }, Qt::QueuedConnection);
  }
}

void load_scene_settings()
{
  QString path = scene_settings_path();
  if (path.isEmpty()) return;
  QSettings settings(path, QSettings::IniFormat);
  sceneDelayEnabled->setChecked(settings.value(QStringLiteral("enabled"), false).toBool());
  sceneDelayMs->setValue(settings.value(QStringLiteral("delay_ms"), 1000).toInt());
  selectedSceneNames.clear();
  for (const QString &name :
       settings.value(QStringLiteral("selected_scenes")).toStringList())
    selectedSceneNames.insert(name);
}

QString syncText(multicam_status_sync_method_t method)
{
  switch (method) {
  case MULTICAM_SYNC_SEI_STAMPER_NTP: return QStringLiteral("SEI-Stamper NTP");
  case MULTICAM_SYNC_MOBLIN_TIMECODE: return QStringLiteral("Moblin HEVC timecode");
  case MULTICAM_SYNC_PTS_FALLBACK: return QStringLiteral("PTS fallback");
  default: return QStringLiteral("Waiting for timestamps");
  }
}

QTableWidgetItem *cell(const QString &text, const QColor &color = QColor())
{
  auto *item = new QTableWidgetItem(text);
  item->setFlags(item->flags() & ~Qt::ItemIsEditable);
  if (color.isValid()) item->setForeground(color);
  return item;
}

bool recent(uint64_t stamp, uint64_t now)
{
  return stamp && now >= stamp && now - stamp <= 2000000000ULL;
}

uint64_t group_spread(const multicam_source_status_t *statuses, size_t count)
{
  uint64_t minimum = UINT64_MAX, maximum = 0;
  for (size_t i = 0; i < count; ++i) {
    if (!statuses[i].last_absolute_unix_ns) return 0;
    minimum = std::min(minimum, statuses[i].last_absolute_unix_ns);
    maximum = std::max(maximum, statuses[i].last_absolute_unix_ns);
  }
  return count ? maximum - minimum : 0;
}

void refresh()
{
  multicam_source_status_t sources[MULTICAM_STATUS_MAX_SOURCES] = {};
  size_t count = multicam_status_snapshot(sources, MULTICAM_STATUS_MAX_SOURCES);
  uint64_t now = os_gettime_ns();
  tableWidget->setRowCount(static_cast<int>(count));

  size_t absolute_count = 0;
  for (size_t i = 0; i < count; ++i)
    if (sources[i].last_absolute_unix_ns && recent(sources[i].last_video_mono_ns, now))
      absolute_count++;
  multicam_source_status_t absolute_sources[MULTICAM_STATUS_MAX_SOURCES] = {};
  size_t absolute_index = 0;
  for (size_t i = 0; i < count; ++i)
    if (sources[i].last_absolute_unix_ns && recent(sources[i].last_video_mono_ns, now))
      absolute_sources[absolute_index++] = sources[i];
  uint64_t spread = group_spread(absolute_sources, absolute_count);

  int ready_count = 0;
  for (size_t i = 0; i < count; ++i) {
    const auto &s = sources[i];
    bool video_ok = recent(s.last_video_mono_ns, now);
    bool audio_ok = !s.has_audio || recent(s.last_audio_mono_ns, now);
    bool packet_ok = recent(s.last_packet_mono_ns, now);
    bool source_ready = multicam_status_source_ready(&s, now);
    if (source_ready) ready_count++;
    QColor good(60, 190, 110), bad(235, 90, 85), warn(235, 175, 60), neutral(160, 165, 175);
    int row = static_cast<int>(i);
    tableWidget->setItem(row, 0, cell(QString::fromUtf8(s.name)));
    tableWidget->setItem(row, 1, cell(s.connected ? QStringLiteral("Connected") : QStringLiteral("Offline"), s.connected ? good : bad));
    tableWidget->setItem(row, 2, cell(video_ok ? QStringLiteral("Live %1×%2").arg(s.width).arg(s.height) : QStringLiteral("No recent video"), video_ok ? good : bad));
    tableWidget->setItem(row, 3, cell(!s.has_audio ? QStringLiteral("No audio stream") : (audio_ok ? QStringLiteral("Live") : QStringLiteral("No recent audio")), !s.has_audio ? neutral : (audio_ok ? good : bad)));
    QColor sync_color = (s.sync_method == MULTICAM_SYNC_SEI_STAMPER_NTP || s.sync_method == MULTICAM_SYNC_MOBLIN_TIMECODE) && s.ntp_fresh ? good : (s.sync_method == MULTICAM_SYNC_PTS_FALLBACK ? warn : neutral);
    tableWidget->setItem(row, 4, cell(QStringLiteral("%1 ms").arg(s.audio_offset_ms)));
    tableWidget->setItem(row, 5, cell(syncText(s.sync_method), sync_color));
    tableWidget->setItem(row, 6, cell(s.ntp_fresh ? QStringLiteral("Fresh") : QStringLiteral("Stale / waiting"), s.ntp_fresh ? good : warn));
    auto *bitrate_1s = cell(!packet_ok ? QStringLiteral("No recent data") : (s.rx_bitrate_bps ? QStringLiteral("%1 Mbps").arg(s.rx_bitrate_bps / 1000000.0, 0, 'f', 2) : QStringLiteral("Measuring")), packet_ok && s.rx_bitrate_bps ? good : warn);
    bitrate_1s->setToolTip(QStringLiteral("Measured demuxed SRT payload over approximately 1 second; excludes transport overhead."));
    tableWidget->setItem(row, 7, bitrate_1s);
    auto *bitrate_5s = cell(s.rx_bitrate_5s_bps ? QStringLiteral("%1 Mbps").arg(s.rx_bitrate_5s_bps / 1000000.0, 0, 'f', 2) : QStringLiteral("Measuring"), packet_ok && s.rx_bitrate_5s_bps ? good : warn);
    bitrate_5s->setToolTip(QStringLiteral("Measured demuxed SRT payload averaged over approximately 5 seconds; excludes transport overhead."));
    tableWidget->setItem(row, 8, bitrate_5s);
    tableWidget->setItem(row, 9, cell(s.fps_milli ? QStringLiteral("%1 fps").arg(s.fps_milli / 1000.0, 0, 'f', 1) : QStringLiteral("—")));
    QString delta = QStringLiteral("—");
    QColor delta_color = neutral;
    if (s.last_absolute_unix_ns && absolute_count > 1) {
      const int64_t d = static_cast<int64_t>(s.last_absolute_unix_ns) -
                        static_cast<int64_t>(absolute_sources[0].last_absolute_unix_ns);
      delta = QStringLiteral("%1 ms").arg(d / 1000000.0, 0, 'f', 1);
      switch (multicam_status_sync_quality(spread)) {
      case MULTICAM_SYNC_QUALITY_EXCELLENT: delta_color = QColor(70, 150, 255); break;
      case MULTICAM_SYNC_QUALITY_GOOD: delta_color = good; break;
      case MULTICAM_SYNC_QUALITY_WARNING: delta_color = warn; break;
      case MULTICAM_SYNC_QUALITY_BAD: delta_color = bad; break;
      }
    }
    tableWidget->setItem(row, 10, cell(video_ok ? QStringLiteral("%1 ms").arg(s.video_offset_ns / 1000000.0, 0, 'f', 1) : QStringLiteral("—")));
    tableWidget->setItem(row, 11, cell(delta, delta_color));
    tableWidget->setItem(row, 12, cell(QString::number(s.decode_errors),
                                      s.decode_errors ? bad : good));
    tableWidget->item(row, 12)->setText(
        QStringLiteral("V %1 / A %2").arg(s.decode_errors).arg(s.audio_decode_errors));
    if (s.audio_decode_errors)
      tableWidget->item(row, 12)->setForeground(bad);
  }

  uint64_t spread_ns = 0;
  bool aligned = absolute_count >= 2 &&
      multicam_status_group_aligned(absolute_sources, absolute_count, now,
                                    kAlignmentToleranceNs, &spread_ns);
  summaryLabel->setText(QStringLiteral("Receivers: %1 | Preflight: %2 | Scheduled output spread: %3 ms")
                            .arg(count).arg(count && ready_count == static_cast<int>(count) && aligned ? QStringLiteral("READY") : QStringLiteral("CHECK"))
                            .arg(absolute_count > 1 ? spread_ns / 1000000.0 : 0.0, 0, 'f', 1));
}

void runPreflight()
{
  multicam_source_status_t sources[MULTICAM_STATUS_MAX_SOURCES] = {};
  size_t count = multicam_status_snapshot(sources, MULTICAM_STATUS_MAX_SOURCES);
  uint64_t now = os_gettime_ns(), spread = 0;
  bool all_ready = count > 0;
  for (size_t i = 0; i < count; ++i)
    all_ready = all_ready && multicam_status_source_ready(&sources[i], now);
  bool aligned = count >= 2 && all_ready && multicam_status_group_aligned(
      sources, count, now, kAlignmentToleranceNs, &spread);
  const QString result = aligned
      ? QStringLiteral("READY: receivers have recent video, expected audio flow, measured RX bitrate, fresh NTP and absolute timestamps. Estimated scheduled output spread is %1 ms (strict preflight target ≤16.67 ms). Audio lip-sync still requires playback verification.").arg(spread / 1000000.0, 0, 'f', 1)
      : QStringLiteral("CHECK: preflight did not pass. Verify every receiver is connected, video/audio are current, RX bitrate is measured, NTP is fresh, metadata is absolute (not PTS fallback), and estimated scheduled output spread is ≤16.67 ms.");
  QMessageBox::information(dockWidget, QStringLiteral("Multicam preflight"), result);
}

void exportReport()
{
  multicam_source_status_t sources[MULTICAM_STATUS_MAX_SOURCES] = {};
  size_t count = multicam_status_snapshot(sources, MULTICAM_STATUS_MAX_SOURCES);
  uint64_t now = os_gettime_ns(), spread = 0;
  QJsonObject root;
  root.insert(QStringLiteral("generated_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
  root.insert(QStringLiteral("redaction"), QStringLiteral("Source names, SRT URLs, stream IDs, hostnames, and credentials are omitted."));
  QJsonArray cameras;
  bool all_ready = count > 0;
  for (size_t i = 0; i < count; ++i) {
    const auto &s = sources[i];
    bool ready = multicam_status_source_ready(&s, now);
    all_ready = all_ready && ready;
    QJsonObject item;
    item.insert(QStringLiteral("id"), QStringLiteral("Camera %1").arg(i + 1));
    item.insert(QStringLiteral("connected"), s.connected);
    item.insert(QStringLiteral("codec"), QString::fromUtf8(s.codec));
    item.insert(QStringLiteral("video_recent"), recent(s.last_video_mono_ns, now));
    item.insert(QStringLiteral("packet_recent"), recent(s.last_packet_mono_ns, now));
    item.insert(QStringLiteral("audio_expected"), s.has_audio);
    item.insert(QStringLiteral("audio_offset_ms"), s.audio_offset_ms);
    item.insert(QStringLiteral("audio_recent"), !s.has_audio || recent(s.last_audio_mono_ns, now));
    item.insert(QStringLiteral("sync_method"), syncText(s.sync_method));
    item.insert(QStringLiteral("ntp_fresh"), s.ntp_fresh);
    item.insert(QStringLiteral("demuxed_rx_payload_bps_1s"), static_cast<qint64>(s.rx_bitrate_bps));
    item.insert(QStringLiteral("demuxed_rx_payload_bps_5s_average"), static_cast<qint64>(s.rx_bitrate_5s_bps));
    item.insert(QStringLiteral("video_pts_offset_ns"), static_cast<qint64>(s.video_offset_ns));
    item.insert(QStringLiteral("observed_input_latency_ms"),
                s.observed_latency_ns / 1000000.0);
    item.insert(QStringLiteral("receiver_buffer_wait_ms"),
                s.playout_delay_ns / 1000000.0);
    item.insert(QStringLiteral("fps"), s.fps_milli / 1000.0);
    item.insert(QStringLiteral("ready"), ready);
    item.insert(QStringLiteral("decode_errors_total"), static_cast<qint64>(s.decode_errors));
    item.insert(QStringLiteral("audio_decode_errors_total"),
                static_cast<qint64>(s.audio_decode_errors));
    cameras.append(item);
  }
  bool aligned = count >= 2 && all_ready && multicam_status_group_aligned(sources, count, now,
                                                              kAlignmentToleranceNs, &spread);
  root.insert(QStringLiteral("cameras"), cameras);
  root.insert(QStringLiteral("preflight_ready"), aligned);
  root.insert(QStringLiteral("audio_sync_verified"), false);
  root.insert(QStringLiteral("absolute_timestamp_spread_ns"), static_cast<qint64>(spread));
  QString path = QFileDialog::getSaveFileName(dockWidget, QStringLiteral("Export redacted diagnostics"),
                                               QStringLiteral("multicam-diagnostics.json"),
                                               QStringLiteral("JSON files (*.json)"));
  if (path.isEmpty()) return;
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    QMessageBox::warning(dockWidget, QStringLiteral("Export failed"), file.errorString());
    return;
  }
  file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
  file.close();
  QMessageBox::information(dockWidget, QStringLiteral("Diagnostics exported"),
                            QStringLiteral("Redacted report saved."));
}

} // namespace

extern "C" void multicam_sync_dock_load(void)
{
  if (dockWidget) return;
  auto *widget = new QWidget(static_cast<QWidget *>(obs_frontend_get_main_window()));
  widget->setMinimumSize(900, 260);
  auto *layout = new QVBoxLayout(widget);
  summaryLabel = new QLabel(QStringLiteral("Waiting for SEI Receiver sources…"), widget);
  layout->addWidget(summaryLabel);

  auto *sceneDelayRow = new QHBoxLayout();
  sceneDelayEnabled = new QCheckBox(QStringLiteral("Delay selected scene changes"), widget);
  sceneDelayMs = new QSpinBox(widget);
  sceneDelayMs->setRange(0, 30000);
  sceneDelayMs->setSingleStep(100);
  sceneDelayMs->setSuffix(QStringLiteral(" ms"));
  sceneDelayScenes = new QListWidget(widget);
  sceneDelayScenes->setMaximumHeight(110);
  load_scene_settings();
  sceneDelayRow->addWidget(sceneDelayEnabled);
  sceneDelayRow->addWidget(new QLabel(QStringLiteral("Delay:"), widget));
  sceneDelayRow->addWidget(sceneDelayMs);
  sceneDelayRow->addStretch(1);
  layout->addLayout(sceneDelayRow);
  layout->addWidget(new QLabel(QStringLiteral("Scenes affected when delay is enabled:"), widget));
  layout->addWidget(sceneDelayScenes);
  QObject::connect(sceneDelayEnabled, &QCheckBox::toggled, widget,
                   [](bool enabled) {
    if (!enabled) apply_pending_scene();
    save_scene_settings();
  });
  QObject::connect(sceneDelayMs, qOverload<int>(&QSpinBox::valueChanged), widget,
                   [](int delay_ms) {
    if (pendingProgramScene && sceneDelayTimer) {
      if (delay_ms <= 0)
        apply_pending_scene();
      else
        sceneDelayTimer->start(delay_ms);
    }
    save_scene_settings();
  });
  QObject::connect(sceneDelayScenes, &QListWidget::itemChanged, widget,
                   [](QListWidgetItem *item) {
    if (item->checkState() == Qt::Checked)
      selectedSceneNames.insert(item->text());
    else
      selectedSceneNames.remove(item->text());
    save_scene_settings();
  });
  refresh_scene_list();

  tableWidget = new QTableWidget(0, 13, widget);
  tableWidget->setHorizontalHeaderLabels({QStringLiteral("Source"), QStringLiteral("Link"),
      QStringLiteral("Video"), QStringLiteral("Audio"), QStringLiteral("Audio offset"), QStringLiteral("Sync source"),
      QStringLiteral("NTP"), QStringLiteral("RX payload (1 s)"),
      QStringLiteral("RX payload (5 s avg)"),
      QStringLiteral("FPS"), QStringLiteral("Video PTS offset"), QStringLiteral("Δ camera"),
      QStringLiteral("Decode errors (video/audio)")});
  tableWidget->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  tableWidget->horizontalHeader()->setStretchLastSection(true);
  tableWidget->setEditTriggers(QAbstractItemView::NoEditTriggers);
  tableWidget->setSelectionMode(QAbstractItemView::NoSelection);
  layout->addWidget(tableWidget);
  auto *buttons = new QHBoxLayout();
  auto *preflight = new QPushButton(QStringLiteral("Run preflight"), widget);
  auto *report = new QPushButton(QStringLiteral("Export redacted report"), widget);
  buttons->addWidget(preflight);
  buttons->addWidget(report);
  buttons->addStretch(1);
  layout->addLayout(buttons);
  QObject::connect(preflight, &QPushButton::clicked, widget, runPreflight);
  QObject::connect(report, &QPushButton::clicked, widget, exportReport);
  sceneDelayTimer = new QTimer(widget);
  sceneDelayTimer->setSingleShot(true);
  QObject::connect(sceneDelayTimer, &QTimer::timeout, widget, apply_pending_scene);
  auto *timer = new QTimer(widget);
  QObject::connect(timer, &QTimer::timeout, widget, refresh);
  timer->start(500);
  refresh();
  if (obs_frontend_add_dock_by_id(kDockId, "Multicam Sync Center", widget)) {
    dockWidget = widget;
    lastProgramScene = obs_frontend_get_current_scene();
    obs_frontend_add_event_callback(frontend_event, nullptr);
  } else {
    delete widget;
  }
}

extern "C" void multicam_sync_dock_unload(void)
{
  if (!dockWidget) return;
  obs_frontend_remove_event_callback(frontend_event, nullptr);
  cancel_pending_scene();
  if (lastProgramScene) obs_source_release(lastProgramScene);
  lastProgramScene = nullptr;
  sceneDelayTimer = nullptr;
  sceneDelayEnabled = nullptr;
  sceneDelayMs = nullptr;
  sceneDelayScenes = nullptr;
  obs_frontend_remove_dock(kDockId);
  dockWidget = nullptr;
  tableWidget = nullptr;
  summaryLabel = nullptr;
}


