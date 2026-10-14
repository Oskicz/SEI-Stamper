#include "multicam-sync-dock.h"
#include "multicam-status.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QDateTime>
#include <QColor>
#include <QAbstractItemView>
#include <QFile>
#include <QFileDialog>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <algorithm>

namespace {
constexpr char kDockId[] = "sei-stamper.multicam-sync-center";
constexpr uint64_t kAlignmentToleranceNs = 16666667ULL;
QWidget *dockWidget;
QTableWidget *tableWidget;
QLabel *summaryLabel;

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
    tableWidget->setItem(row, 7, cell(!packet_ok ? QStringLiteral("No recent data") : (s.rx_bitrate_bps ? QStringLiteral("%1 Mbps").arg(s.rx_bitrate_bps / 1000000.0, 0, 'f', 2) : QStringLiteral("Measuring")), packet_ok && s.rx_bitrate_bps ? good : warn));
    tableWidget->setItem(row, 8, cell(s.rx_bitrate_5s_bps ? QStringLiteral("%1 Mbps").arg(s.rx_bitrate_5s_bps / 1000000.0, 0, 'f', 2) : QStringLiteral("Measuring"), packet_ok && s.rx_bitrate_5s_bps ? good : warn));
    tableWidget->setItem(row, 9, cell(s.fps_milli ? QStringLiteral("%1 fps").arg(s.fps_milli / 1000.0, 0, 'f', 1) : QStringLiteral("—")));
    QString delta = QStringLiteral("—");
    QColor delta_color = neutral;
    if (s.last_absolute_unix_ns && absolute_count > 1) {
      const int64_t d = static_cast<int64_t>(s.last_absolute_unix_ns) - static_cast<int64_t>(absolute_sources[0].last_absolute_unix_ns);
      delta = QStringLiteral("%1 ms").arg(d / 1000000.0, 0, 'f', 1);
      delta_color = spread <= kAlignmentToleranceNs ? good : bad;
    }
    tableWidget->setItem(row, 10, cell(video_ok ? QStringLiteral("%1 ms").arg(s.video_offset_ns / 1000000.0, 0, 'f', 1) : QStringLiteral("—")));
    tableWidget->setItem(row, 11, cell(delta, delta_color));
    tableWidget->setItem(row, 12, cell(QString::number(s.decode_errors),
                                      s.decode_errors ? bad : good));
  }

  uint64_t spread_ns = 0;
  bool aligned = absolute_count >= 2 &&
      multicam_status_group_aligned(absolute_sources, absolute_count, now,
                                    kAlignmentToleranceNs, &spread_ns);
  summaryLabel->setText(QStringLiteral("Receivers: %1 | Preflight: %2 | Absolute group spread: %3 ms")
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
      ? QStringLiteral("READY: receivers have recent video, expected audio flow, measured RX bitrate, fresh NTP and absolute timestamps. Inter-camera timestamp spread is %1 ms (target ≤16.67 ms). Audio lip-sync still requires playback verification.").arg(spread / 1000000.0, 0, 'f', 1)
      : QStringLiteral("CHECK: preflight did not pass. Verify every receiver is connected, video/audio are current, RX bitrate is measured, NTP is fresh, metadata is absolute (not PTS fallback), and camera timestamp spread is ≤16.67 ms.");
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
    item.insert(QStringLiteral("fps"), s.fps_milli / 1000.0);
    item.insert(QStringLiteral("ready"), ready);
    item.insert(QStringLiteral("decode_errors_total"), static_cast<qint64>(s.decode_errors));
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
  tableWidget = new QTableWidget(0, 13, widget);
  tableWidget->setHorizontalHeaderLabels({QStringLiteral("Source"), QStringLiteral("Link"),
      QStringLiteral("Video"), QStringLiteral("Audio"), QStringLiteral("Audio offset"), QStringLiteral("Sync source"),
      QStringLiteral("NTP"), QStringLiteral("RX payload (1 s)"),
      QStringLiteral("RX payload (5 s avg)"),
      QStringLiteral("FPS"), QStringLiteral("Video PTS offset"), QStringLiteral("Δ camera"),
      QStringLiteral("Decode errors")});
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
  auto *timer = new QTimer(widget);
  QObject::connect(timer, &QTimer::timeout, widget, refresh);
  timer->start(500);
  refresh();
  if (obs_frontend_add_dock_by_id(kDockId, "Multicam Sync Center", widget))
    dockWidget = widget;
  else
    delete widget;
}

extern "C" void multicam_sync_dock_unload(void)
{
  if (!dockWidget) return;
  obs_frontend_remove_dock(kDockId);
  dockWidget = nullptr;
  tableWidget = nullptr;
  summaryLabel = nullptr;
}

