/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#ifndef PANEL_CLAUDE_SETTINGS_H
#define PANEL_CLAUDE_SETTINGS_H

#include <widgets/resettable_panel.h>

class wxTextCtrl;
class wxComboBox;
class wxStaticText;

/**
 * Preferences page for the Claude chat panel: API key (stored in the OS keychain),
 * model, and the paths used to run the KiCad MCP server.
 */
class PANEL_CLAUDE_SETTINGS : public RESETTABLE_PANEL
{
public:
    PANEL_CLAUDE_SETTINGS( wxWindow* aParent );

    bool TransferDataToWindow() override;
    bool TransferDataFromWindow() override;
    void ResetPanel() override;

private:
    void updateKeyStatus();

    wxTextCtrl*   m_apiKey;
    wxStaticText* m_keyStatus;
    wxComboBox*   m_model;
    wxTextCtrl*   m_node;
    wxTextCtrl*   m_server;
    wxTextCtrl*   m_python;
    wxTextCtrl*   m_pythonPath;
    bool          m_deleteKey;
};

#endif // PANEL_CLAUDE_SETTINGS_H
