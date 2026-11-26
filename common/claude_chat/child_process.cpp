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

#include <claude_chat/child_process.h>

#include <chrono>
#include <thread>

#include <wx/process.h>
#include <wx/stream.h>

static const size_t STDERR_TAIL_MAX = 8192;


static long long nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>( steady_clock::now().time_since_epoch() ).count();
}


/// wxProcess that deletes itself only when orphaned (killed while still running).
class CHILD_WX_PROCESS : public wxProcess
{
public:
    CHILD_WX_PROCESS( std::shared_ptr<std::atomic<bool>> aExited ) :
            wxProcess( wxPROCESS_REDIRECT ),
            m_exited( std::move( aExited ) ),
            m_orphaned( false )
    {}

    void OnTerminate( int aPid, int aStatus ) override
    {
        *m_exited = true;

        if( m_orphaned )
            delete this;
    }

    std::shared_ptr<std::atomic<bool>> m_exited;
    bool                               m_orphaned;
};


CLAUDE_CHILD_PROCESS::CLAUDE_CHILD_PROCESS() :
        m_process( nullptr ),
        m_pid( 0 ),
        m_exited( std::make_shared<std::atomic<bool>>( true ) )
{
}


CLAUDE_CHILD_PROCESS::~CLAUDE_CHILD_PROCESS()
{
    Kill();
}


bool CLAUDE_CHILD_PROCESS::IsRunning() const
{
    return m_process && !*m_exited;
}


bool CLAUDE_CHILD_PROCESS::Start( const std::vector<wxString>& aArgv, wxExecuteEnv& aEnv,
                                  wxString& aError )
{
    Kill();

    std::vector<const wchar_t*> argv;

    for( const wxString& arg : aArgv )
        argv.push_back( arg.wc_str() );

    argv.push_back( nullptr );

    m_exited = std::make_shared<std::atomic<bool>>( false );
    m_process = new CHILD_WX_PROCESS( m_exited );
    m_stdoutBuf.clear();
    {
        std::lock_guard<std::mutex> lock( m_stderrMutex );
        m_stderrTail.clear();
    }

    int flags = wxEXEC_ASYNC | wxEXEC_HIDE_CONSOLE | wxEXEC_MAKE_GROUP_LEADER;
    m_pid = wxExecute( argv.data(), flags, m_process, &aEnv );

    if( m_pid <= 0 )
    {
        delete m_process;
        m_process = nullptr;
        *m_exited = true;
        aError = wxString::Format( wxS( "%s" ), aArgv.empty() ? wxString() : aArgv[0] );
        return false;
    }

    return true;
}


bool CLAUDE_CHILD_PROCESS::WriteAllAndClose( const std::string& aData )
{
    if( !IsRunning() )
        return false;

    wxOutputStream* out = m_process->GetOutputStream();

    if( !out )
        return false;

    out->Write( aData.data(), aData.size() );
    bool ok = out->LastWrite() == aData.size();
    m_process->CloseOutput();
    return ok;
}


void CLAUDE_CHILD_PROCESS::Kill()
{
    if( !m_process )
        return;

    if( !*m_exited )
    {
        m_process->m_orphaned = true;
        wxProcess::Kill( m_pid, wxSIGKILL, wxKILL_CHILDREN );
    }
    else
    {
        delete m_process;
    }

    m_process = nullptr;
    m_pid = 0;
}


void CLAUDE_CHILD_PROCESS::drainStderr()
{
    std::lock_guard<std::mutex> lock( m_stderrMutex );

    if( !m_process )
        return;

    wxInputStream* err = m_process->GetErrorStream();

    if( !err )
        return;

    char buf[4096];

    while( err->CanRead() )
    {
        err->Read( buf, sizeof( buf ) );
        size_t n = err->LastRead();

        if( n == 0 )
            break;

        m_stderrTail.append( buf, n );
    }

    if( m_stderrTail.size() > STDERR_TAIL_MAX )
        m_stderrTail.erase( 0, m_stderrTail.size() - STDERR_TAIL_MAX );
}


wxString CLAUDE_CHILD_PROCESS::StderrTail()
{
    drainStderr();
    std::lock_guard<std::mutex> lock( m_stderrMutex );
    return wxString::FromUTF8( m_stderrTail );
}


CLAUDE_CHILD_PROCESS::READ_RESULT CLAUDE_CHILD_PROCESS::ReadLine( std::string& aLine,
                                                                  int aTimeoutSec,
                                                                  const std::atomic<bool>& aCancel )
{
    long long deadline = nowMs() + aTimeoutSec * 1000LL;
    char      buf[8192];

    while( true )
    {
        size_t nl = m_stdoutBuf.find( '\n' );

        if( nl != std::string::npos )
        {
            aLine = m_stdoutBuf.substr( 0, nl );
            m_stdoutBuf.erase( 0, nl + 1 );

            if( !aLine.empty() && aLine.back() == '\r' )
                aLine.pop_back();

            return READ_RESULT::LINE;
        }

        if( aCancel )
            return READ_RESULT::CANCELLED;

        drainStderr();

        wxInputStream* in = m_process ? m_process->GetInputStream() : nullptr;
        bool           gotData = false;

        if( in && in->CanRead() )
        {
            in->Read( buf, sizeof( buf ) );
            size_t n = in->LastRead();

            if( n > 0 )
            {
                m_stdoutBuf.append( buf, n );
                gotData = true;
            }
        }

        if( gotData )
            continue;

        if( *m_exited || !in )
        {
            // Return a last line without trailing newline, if any
            if( !m_stdoutBuf.empty() )
            {
                aLine = m_stdoutBuf;
                m_stdoutBuf.clear();
                return READ_RESULT::LINE;
            }

            return READ_RESULT::EXITED;
        }

        if( nowMs() > deadline )
            return READ_RESULT::TIMEOUT;

        std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
    }
}
