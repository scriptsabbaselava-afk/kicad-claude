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

#ifndef CLAUDE_CHILD_PROCESS_H
#define CLAUDE_CHILD_PROCESS_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <wx/string.h>
#include <wx/utils.h>

class CHILD_WX_PROCESS;

/**
 * A child process with redirected stdio, read line by line from a worker thread.
 *
 * Start() / Kill() must be called on the GUI thread.  ReadLine() is blocking and is used by
 * one worker thread.  The object must outlive the worker.
 */
class CLAUDE_CHILD_PROCESS
{
public:
    CLAUDE_CHILD_PROCESS();
    ~CLAUDE_CHILD_PROCESS();

    bool Start( const std::vector<wxString>& aArgv, wxExecuteEnv& aEnv, wxString& aError );

    /// Write aData to the child's stdin, then close it (EOF).
    bool WriteAllAndClose( const std::string& aData );

    enum class READ_RESULT
    {
        LINE,
        EXITED,
        CANCELLED,
        TIMEOUT
    };

    READ_RESULT ReadLine( std::string& aLine, int aTimeoutSec, const std::atomic<bool>& aCancel );

    bool     IsRunning() const;
    void     Kill();
    wxString StderrTail();

private:
    void drainStderr();

    CHILD_WX_PROCESS*                  m_process;
    long                               m_pid;
    std::shared_ptr<std::atomic<bool>> m_exited;
    std::string                        m_stdoutBuf;
    std::mutex                         m_stderrMutex;
    std::string                        m_stderrTail;
};

#endif // CLAUDE_CHILD_PROCESS_H
