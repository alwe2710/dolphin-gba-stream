// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/GBAStreamSlotDialog.h"

#ifdef HAS_LIBMGBA

#include <QDialogButtonBox>
#include <QFontMetrics>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "Common/Config/Config.h"
#include "Core/Config/MainSettings.h"

#include "DolphinQt/Settings/GameCubePane.h"

GBAStreamSlotDialog::GBAStreamSlotDialog(int port, QWidget* parent) : QDialog(parent), m_port{port}
{
  CreateLayout();
  ConnectWidgets();
  RefreshRomLabel();
}

void GBAStreamSlotDialog::CreateLayout()
{
  setWindowTitle(tr("GBA (Client-Stream) at Port %1").arg(m_port + 1));

  m_layout = new QVBoxLayout();
  m_rom_label = new QLabel();
  m_browse_button = new QPushButton(tr("Load ROM..."));
  m_clear_button = new QPushButton(tr("Clear Slot"));
  m_button_box = new QDialogButtonBox(QDialogButtonBox::Ok);

  m_layout->addWidget(m_rom_label);
  m_layout->addWidget(m_browse_button);
  m_layout->addWidget(m_clear_button);
  m_layout->addWidget(m_button_box);

  setLayout(m_layout);
}

void GBAStreamSlotDialog::ConnectWidgets()
{
  connect(m_browse_button, &QPushButton::clicked, this, &GBAStreamSlotDialog::BrowseRom);
  connect(m_clear_button, &QPushButton::clicked, this, &GBAStreamSlotDialog::ClearRom);
  connect(m_button_box, &QDialogButtonBox::accepted, this, &GBAStreamSlotDialog::accept);
}

void GBAStreamSlotDialog::BrowseRom()
{
  // Same file dialog (filter, title convention) GameCubePane's own "GBA
  // Settings" box uses for the identical Config::MAIN_GBA_ROM_PATHS[port]
  // setting -- this dialog is just a second, port-scoped door to it.
  const std::string rom = GameCubePane::GetOpenGBARom(tr("Port %1").arg(m_port + 1).toStdString());
  if (rom.empty())
    return;  // Cancelled.

  Config::SetBaseOrCurrent(Config::MAIN_GBA_ROM_PATHS[m_port], rom);
  RefreshRomLabel();
}

void GBAStreamSlotDialog::ClearRom()
{
  Config::SetBaseOrCurrent(Config::MAIN_GBA_ROM_PATHS[m_port], std::string());
  RefreshRomLabel();
}

void GBAStreamSlotDialog::RefreshRomLabel()
{
  const std::string rom = Config::Get(Config::MAIN_GBA_ROM_PATHS[m_port]);
  m_clear_button->setEnabled(!rom.empty());

  if (rom.empty())
  {
    m_rom_label->setText(tr("No ROM loaded -- this slot starts at the GBA BIOS menu, same as an "
                             "unaltered GC_GBA_STREAM port always has."));
    m_rom_label->setToolTip(QString());
    return;
  }

  // Long absolute paths (the common case, from a file picker) would
  // otherwise force this dialog wider than any of its buttons need it to
  // be -- elide to the filename's own directory context and rely on the
  // tooltip for the exact full path, same trade-off GameCubePane's
  // QLineEdit-based rows sidestep only by scrolling instead.
  const QString full_path = QString::fromStdString(rom);
  const QFontMetrics metrics(m_rom_label->font());
  m_rom_label->setText(tr("ROM: %1")
                            .arg(metrics.elidedText(full_path, Qt::ElideMiddle, 420)));
  m_rom_label->setToolTip(full_path);
}

#endif  // HAS_LIBMGBA
