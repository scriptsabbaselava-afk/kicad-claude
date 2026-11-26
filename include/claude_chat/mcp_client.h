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

#ifndef MCP_CLIENT_H
#define MCP_CLIENT_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>
#include <wx/string.h>
#include <wx/utils.h>

struct CLAUDE_CHAT_CONFIG;
class MCP_PROCESS;

/**
 * Minimal Model Context Protocol client speaking JSON-RPC over the stdio of a child process.
 *
 * Threading: Start() / Stop() / DrainStderr() must be called from the GUI thread
 * (wxExecute requirement).  Initialize() / ListTools() / CallTool() are blocking and are
 * meant to be called from one worker thread at a time.
 */
class MCP_CLIENT
{
public:
    MCP_CLIENT();
    ~MCP_CLIENT();

    bool Start( const CLAUDE_CHAT_CONFIG& aConfig, const wxString& aToolboxes, wxString& aError );

    /// Environment (PATH, KICAD_PYTHON, PYTHONPATH, toolboxes...) and working directory the
    /// KiCad MCP server needs.  Also used when Claude Code launches the server itself.
    static void BuildServerEnv( const CLAUDE_CHAT_CONFIG& aConfig, const wxString& aToolboxes,
                                wxExecuteEnv& aEnv );
    void Stop();
    bool IsRunning() const;
    bool IsInitialized() const { return m_initialized; }

    bool Initialize( const std::atomic<bool>& aCancel, wxString& aError );
    bool ListTools( nlohmann::json& aTools, const std::atomic<bool>& aCancel, wxString& aError );
    bool CallTool( const std::string& aName, const nlohmann::json& aArgs, nlohmann::json& aResult,
                   const std::atomic<bool>& aCancel, wxString& aError );

    /// True (and reset) when the server announced that its tool list changed.
    bool ConsumeToolsChanged() { return m_toolsChanged.exchange( false ); }

    /// Read whatever the server wrote to stderr so its pipe never fills up.
    void DrainStderr();

    /// The last few KB of the server's stderr, for error messages.
    wxString RecentStderr();

private:
    bool request( const std::string& aMethod, const nlohmann::json& aParams, nlohmann::json& aResult,
                  int aTimeoutSec, const std::atomic<bool>& aCancel, wxString& aError );
    bool send( const nlohmann::json& aMsg, wxString& aError );
    bool readLine( std::string& aLine, long long aDeadlineMs, const std::atomic<bool>& aCancel,
                   wxString& aError );
    wxString deadServerMessage();

    MCP_PROCESS*                       m_process;
    long                               m_pid;
    std::shared_ptr<std::atomic<bool>> m_exited;
    std::string                        m_stdoutBuf;
    std::mutex                         m_stderrMutex;
    std::string                        m_stderrTail;
    int                                m_nextId;
    bool                               m_initialized;
    std::atomic<bool>                  m_toolsChanged;
};

#endif // MCP_CLIENT_H
