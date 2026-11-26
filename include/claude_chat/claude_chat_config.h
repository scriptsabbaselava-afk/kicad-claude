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

#ifndef CLAUDE_CHAT_CONFIG_H
#define CLAUDE_CHAT_CONFIG_H

#include <wx/string.h>

/// French UI literals are UTF-8 in the source; convert them explicitly on every platform.
#define TR_UTF8( s ) wxString::FromUTF8( s )

/**
 * Settings of the Claude chat panel.
 *
 * Non-secret values live in "claude_chat.json" in the KiCad user settings folder.
 * The Anthropic API key is NEVER written there: it is kept in the OS secret store
 * (macOS Keychain, Windows Credential Manager, libsecret on Linux).
 */
struct CLAUDE_CHAT_CONFIG
{
    wxString m_model;        ///< Claude model id
    wxString m_node;         ///< Node.js executable used to run the MCP server
    wxString m_server;       ///< Path to the MCP server entry point (dist/index.js)
    wxString m_kicadPython;  ///< Python interpreter given to the MCP server (KICAD_PYTHON)
    wxString m_pythonPath;   ///< PYTHONPATH given to the MCP server (may be empty)
    wxString m_toolboxes;    ///< Extra MCP toolboxes opened at start (KICAD_MCP_TOOLBOXES)

    /// Load from disk, filling every empty value with an auto-detected default.
    static CLAUDE_CHAT_CONFIG Load();

    bool Save() const;

    static wxString ConfigFilePath();

    /// Folder where the installer puts node / the MCP server (~/kicad-claude or %LOCALAPPDATA%\KiCadClaude).
    static wxString InstallBaseDir();

    static bool     GetApiKey( wxString& aKey );
    static bool     SetApiKey( const wxString& aKey );
    static bool     DeleteApiKey();

    static constexpr const char* DEFAULT_MODEL = "claude-opus-5-5";
};

#endif // CLAUDE_CHAT_CONFIG_H
