#include "multicam-sync-dock.h"
#include "multicam-status.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QDateTime>
#include <QColor>
#include <QAbstractItemView>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QLabel>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QSettings>
#include <QSet>
#include <QThread>
#include <QTableWidget>
#include <QToolButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

extern "C" void sei_receiver_cleanup_playout_filters(void);

namespace {
constexpr char kDockId[] = "sei-stamper.multicam-sync-center";
constexpr uint64_t kAlignmentToleranceNs = 16666667ULL;
QWidget *dockWidget;
QTableWidget *tableWidget;
QLabel *summaryLabel;
QToolButton *sceneDelayToggle;
QToolButton *sceneDelayChoose;
QMenu *sceneDelayMenu;
QTimer *sceneDelayTimer;
obs_source_t *lastProgramScene;
obs_source_t *pendingProgramScene;
QSet<QString> selectedSceneNames;
bool sceneDelayEnabled;

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
  QString path = scene_settings_path();
  if (path.isEmpty()) return;
  QDir().mkpath(QFileInfo(path).absolutePath());
  QSettings settings(path, QSettings::IniFormat);
  settings.setValue(QStringLiteral("enabled"), sceneDelayEnabled);
  QStringList names;
  for (const QString &name : selectedSceneNames) names.push_back(name);
  settings.setValue(QStringLiteral("selected_scenes"), names);
  settings.sync();
}

void update_scene_delay_toggle()
{
  if (!sceneDelayToggle) return;
  sceneDelayToggle->setChecked(sceneDelayEnabled);
  sceneDelayToggle->setText(sceneDelayEnabled
                                ? QStringLiteral("Scene delay: ON")
                                : QStringLiteral("Scene delay: OFF"));
}

void load_scene_settings()
{
  QString path = scene_settings_path();
  if (path.isEmpty()) return;
  QSettings settings(path, QSettings::IniFormat);
  sceneDelayEnabled = settings.value(QStringLiteral("enabled"), false).toBool();
  const QStringList names = settings.value(QStringLiteral("selected_scenes")).toStringList();
  selectedSceneNames.clear();
  for (const QString &name : names) selectedSceneNames.insert(name);
  update_scene_delay_toggle();
}

struct SceneReceiverNames {
  QStringList names;
  QSet<obs_source_t *> visited;
};

bool collect_scene_receivers(obs_scene_t *scene, obs_sceneitem_t *item,
                             void *opaque)
{
  auto *collected = static_cast<SceneReceiverNames *>(opaque);
  obs_source_t *source = obs_sceneitem_get_source(item);
  if (!source || collected->visited.contains(source)) return true;
  collected->visited.insert(source);
  const char *id = obs_source_get_id(source);
  if (id && strcmp(id, "sei_receiver_source") == 0)
    collected->names.push_back(QString::fromUtf8(obs_source_get_name(source)));
  obs_scene_t *nested = obs_group_or_scene_from_source(source);
  if (nested) obs_scene_enum_items(nested, collect_scene_receivers, collected);
  UNUSED_PARAMETER(scene);
  return true;
}

uint64_t scene_delay_ns(obs_source_t *scene_source)
{
  obs_scene_t *scene = scene_source ? obs_scene_from_source(scene_source) : nullptr;
  if (!scene) return 0;
  SceneReceiverNames collected;
  collected.visited.insert(scene_source);
  obs_scene_enum_items(scene, collect_scene_receivers, &collected);
  if (collected.names.isEmpty()) return 0;

  std::vector<QByteArray> name_bytes;
  std::vector<const char *> names;
  name_bytes.reserve(static_cast<size_t>(collected.names.size()));
  names.reserve(static_cast<size_t>(collected.names.size()));
  for (const QString &name : collected.names)
    name_bytes.push_back(name.toUtf8());
  for (const QByteArray &name : name_bytes) names.push_back(name.constData());

  multicam_source_status_t statuses[MULTICAM_STATUS_MAX_SOURCES] = {};
  size_t count = multicam_status_snapshot(statuses, MULTICAM_STATUS_MAX_SOURCES);
  return multicam_status_selected_scene_delay_ns(
      statuses, count, names.data(), names.size(), os_gettime_ns());
}

void refresh_scene_menu()
{
  if (!sceneDelayMenu) return;
  sceneDelayMenu->clear();
  obs_frontend_source_list scenes = {};
  obs_frontend_get_scenes(&scenes);
  for (size_t i = 0; i < scenes.sources.num; ++i) {
    obs_source_t *source = scenes.sources.array[i];
    const QString name = QString::fromUtf8(obs_source_get_name(source));
    auto *action = sceneDelayMenu->addAction(name);
    action->setCheckable(true);
    action->setChecked(selectedSceneNames.contains(name));
    action->setToolTip(QStringLiteral("Automatic delay: %1 ms")
                           .arg((scene_delay_ns(source) + 500000ULL) / 1000000ULL));
    QObject::connect(action, &QAction::toggled, sceneDelayMenu,
                     [name](bool checked) {
      if (checked) selectedSceneNames.insert(name);
      else selectedSceneNames.remove(name);
      save_scene_settings();
    });
  }
  obs_frontend_source_list_free(&scenes);
}

class ShowSceneMenuOnHover final : public QObject {
public:
  explicit ShowSceneMenuOnHover(QToolButton *button) : QObject(button), button_(button) {}

protected:
  bool eventFilter(QObject *watched, QEvent *event) override
  {
    if (watched == button_ && event->type() == QEvent::Enter) {
      QToolButton *button = button_;
      QTimer::singleShot(180, button, [button] {
        if (button->underMouse() && button->menu() &&
            !button->menu()->isVisible())
          button->showMenu();
      });
    }
    return QObject::eventFilter(watched, event);
  }

private:
  QToolButton *button_;
};

void set_last_program_scene(obs_source_t *scene)
{
  obs_source_t *replacement = scene ? obs_source_get_ref(scene) : nullptr;
  if (lastProgramScene) obs_source_release(lastProgramScene);
  lastProgramScene = replacement;
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
  lastProgramScene = obs_source_get_ref(target);
  obs_frontend_set_current_scene(target);
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
  const uint64_t auto_delay_ns = sceneDelayEnabled &&
          selectedSceneNames.contains(currentName)
      ? scene_delay_ns(current) : 0;
  if (!auto_delay_ns) {
    cancel_pending_scene();
    set_last_program_scene(current);
    return;
  }

  cancel_pending_scene();
  pendingProgramScene = obs_source_get_ref(current);
  if (!pendingProgramScene) return;

  /* Scene-changed is reported after OBS has selected the scene. Restore the
   * old scene synchronously in the same UI event, before OBS can render it,
   * then commit the requested scene after its measured source-to-OBS latency. */
  obs_frontend_set_current_scene(lastProgramScene);
  const uint64_t delay_ms = (auto_delay_ns + 999999ULL) / 1000000ULL;
  sceneDelayTimer->start(static_cast<int>(std::min<uint64_t>(
      delay_ms, static_cast<uint64_t>(std::numeric_limits<int>::max()))));
}

void frontend_event(enum obs_frontend_event event, void *opaque)
{
  UNUSED_PARAMETER(opaque);
  QWidget *target = dockWidget;
  if (!target) return;
  if (event == OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED) {
    QMetaObject::invokeMethod(target, [] { refresh_scene_menu(); },
                              Qt::QueuedConnection);
  } else if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING ||
             event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP) {
    QMetaObject::invokeMethod(target, [] {
      cancel_pending_scene();
      if (lastProgramScene) obs_source_release(lastProgramScene);
      lastProgramScene = nullptr;
    }, Qt::QueuedConnection);
  } else if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
    QMetaObject::invokeMethod(target, [] {
      refresh_scene_menu();
      sei_receiver_cleanup_playout_filters();
    }, Qt::QueuedConnection);
  } else if (event == OBS_FRONTEND_EVENT_SCENE_CHANGED) {
    obs_source_t *current = obs_frontend_get_current_scene();
    if (!current) return;
    auto currentRef = std::shared_ptr<obs_source_t>(current, obs_source_release);
    auto dispatch = [currentRef] {
      handle_program_scene_changed(currentRef.get());
    };
    if (QThread::currentThread() == target->thread())
      dispatch();
    else
      QMetaObject::invokeMethod(target, dispatch, Qt::BlockingQueuedConnection);
  }
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
    const uint64_t scheduled = multicam_status_presentation_time_ns(&statuses[i]);
    if (!scheduled) return 0;
    minimum = std::min(minimum, scheduled);
    maximum = std::max(maximum, scheduled);
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
      const int64_t d =
          static_cast<int64_t>(multicam_status_presentation_time_ns(&s)) -
          static_cast<int64_t>(multicam_status_presentation_time_ns(&absolute_sources[0]));
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
  uint64_t capture_min = UINT64_MAX, capture_max = 0;
  QJsonObject root;
  root.insert(QStringLiteral("generated_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
  root.insert(QStringLiteral("redaction"), QStringLiteral("Source names, SRT URLs, stream IDs, hostnames, and credentials are omitted."));
  QJsonArray cameras;
  bool all_ready = count > 0;
  for (size_t i = 0; i < count; ++i) {
    const auto &s = sources[i];
    if (s.last_absolute_unix_ns && recent(s.last_video_mono_ns, now)) {
      capture_min = std::min(capture_min, s.last_absolute_unix_ns);
      capture_max = std::max(capture_max, s.last_absolute_unix_ns);
    }
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
  const uint64_t capture_spread = capture_max >= capture_min
                                      ? capture_max - capture_min : 0;
  root.insert(QStringLiteral("capture_timestamp_spread_ns"),
              static_cast<qint64>(capture_spread));
  root.insert(QStringLiteral("scheduled_output_spread_ns"),
              static_cast<qint64>(spread));
  root.insert(QStringLiteral("absolute_timestamp_spread_ns"),
              static_cast<qint64>(capture_spread));
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
  load_scene_settings();
  sceneDelayMenu = new QMenu(widget);
  sceneDelayToggle = new QToolButton(widget);
  sceneDelayToggle->setCheckable(true);
  sceneDelayToggle->setToolTip(QStringLiteral(
      "Wait for the selected scene's measured source-to-OBS latency."));
  sceneDelayChoose = new QToolButton(widget);
  sceneDelayChoose->setText(QStringLiteral("Scenes ▾"));
  sceneDelayChoose->setToolTip(QStringLiteral(
      "Hover or click to choose scenes that use automatic delay."));
  sceneDelayChoose->setMenu(sceneDelayMenu);
  sceneDelayChoose->setPopupMode(QToolButton::InstantPopup);
  sceneDelayChoose->installEventFilter(new ShowSceneMenuOnHover(sceneDelayChoose));
  update_scene_delay_toggle();
  QObject::connect(sceneDelayToggle, &QToolButton::toggled, widget,
                   [](bool enabled) {
    sceneDelayEnabled = enabled;
    save_scene_settings();
    update_scene_delay_toggle();
  });
  QObject::connect(sceneDelayMenu, &QMenu::aboutToShow, widget, refresh_scene_menu);
  refresh_scene_menu();
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
  buttons->addWidget(sceneDelayToggle);
  buttons->addWidget(sceneDelayChoose);
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
    /* The frontend can finish loading the collection before this dock gets
     * registered on some OBS startup paths. Run cleanup after the UI has
     * returned to its event loop as well as on FINISHED_LOADING, so filters
     * persisted by earlier plugin builds are always collapsed to one. */
    QTimer::singleShot(0, widget, [] {
      sei_receiver_cleanup_playout_filters();
      refresh_scene_menu();
    });
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
  obs_frontend_remove_dock(kDockId);
  dockWidget = nullptr;
  tableWidget = nullptr;
  summaryLabel = nullptr;
  sceneDelayToggle = nullptr;
  sceneDelayChoose = nullptr;
  sceneDelayMenu = nullptr;
  sceneDelayEnabled = false;
}

