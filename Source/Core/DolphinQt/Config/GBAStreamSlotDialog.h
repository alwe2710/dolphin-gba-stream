// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Config::MAIN_GBA_ROM_PATHS (the setting this dialog exists to edit) only
// exists when mGBA is compiled in -- same guard every other GBA-Stream file
// in this fork uses (GBAStreamHost.cpp/GBAStreamLobby.cpp's own #ifdef at
// the top), and the SI device type this dialog is reached from
// (GC_GBA_STREAM) is itself only selectable in the UI under the same
// condition (see GamecubeControllersWidget.cpp's s_gc_types).
#ifdef HAS_LIBMGBA

#include <QDialog>

class QDialogButtonBox;
class QLabel;
class QPushButton;
class QVBoxLayout;

// Shown from GamecubeControllersWidget's "Configure" button for a port set
// to GBA (Client-Stream) -- unlike the generic per-button MappingWindow
// that opens for every other SI device type (including GBA (Integrated),
// see GamecubeControllersWidget::OnGCPadConfigure), a Unison stream client
// has no local buttons to map at all, so that window would be empty/
// meaningless here. What a GBA-Stream port actually needs configuring is
// which ROM this host boots for that slot before a client ever connects --
// same Config::MAIN_GBA_ROM_PATHS[port] the GameCube pane's separate "GBA
// Settings" box already edits (this dialog is a second, port-scoped way to
// reach the exact same setting, not a separate one), read by
// GBAStreamHost/GBAStreamLobby at boot the same way GBA (Integrated) reads
// it for its own emulated cartridge.
class GBAStreamSlotDialog final : public QDialog
{
  Q_OBJECT
public:
  explicit GBAStreamSlotDialog(int port, QWidget* parent = nullptr);

private:
  void CreateLayout();
  void ConnectWidgets();

  void BrowseRom();
  void ClearRom();
  void RefreshRomLabel();

  int m_port;

  QVBoxLayout* m_layout;
  QLabel* m_rom_label;
  QPushButton* m_browse_button;
  QPushButton* m_clear_button;
  QDialogButtonBox* m_button_box;
};

#endif  // HAS_LIBMGBA
