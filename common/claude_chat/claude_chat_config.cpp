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

#include <claude_chat/claude_chat_config.h>

#include <kiplatform/secrets.h>
#include <settings/settings_manager.h>

#include <nlohmann/json.hpp>

#include <wx/filename.h>
#include <wx/ffile.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>

static const wxString SECRET_SERVICE = wxS( "KiCad-Claude" );
static const wxString SECRET_KEY = wxS( "anthropic-api-key" );


wxString CLAUDE_CHAT_CONFIG::ConfigFilePath()
{
    wxFileName fn( SETTINGS_MANAGER::GetUserSettingsPath(), wxS( "claude_chat.json" ) );
    return fn.GetFullPath();
}


wxString CLAUDE_CHAT_CONFIG::InstallBaseDir()
{
    wxString env;

    if( wxGetEnv( wxS( "KICAD_CLAUDE_HOME" ), &env ) && !env.IsEmpty() )
        return env;

#ifdef __WXMSW__
    wxString local;

    if( wxGetEnv( wxS( "LOCALAPPDATA" ), &local ) )
        return local + wxS( "\\KiCadClaude" );
#endif

    return wxGetHomeDir() + wxFileName::GetPathSeparator() + wxS( "kicad-claude" );
}


static wxString firstExisting( const std::vector<wxString>& aCandidates )
{
    for( const wxString& c : aCandidates )
    {
        if( wxFileName::FileExists( c ) )
            return c;
    }

    return wxEmptyString;
}


static wxString defaultNode( const wxString& aBase )
{
#ifdef __WXMSW__
    wxString found = firstExisting( { aBase + wxS( "\\node\\node.exe" ),
                                      wxS( "C:\\Program Files\\nodejs\\node.exe" ) } );
    return found.IsEmpty() ? wxString( wxS( "node.exe" ) ) : found;
#else
    // GUI apps on macOS do not inherit the shell PATH, so look in the usual places.
    wxString found = firstExisting( { aBase + wxS( "/node/bin/node" ),
                                      wxS( "/opt/homebrew/bin/node" ),
                                      wxS( "/usr/local/bin/node" ),
                                      wxS( "/usr/bin/node" ) } );
    return found.IsEmpty() ? wxString( wxS( "node" ) ) : found;
#endif
}


static wxString defaultKicadPython()
{
    wxFileName exe( wxStandardPaths::Get().GetExecutablePath() );

#if defined( __WXMAC__ )
    // .../KiCad.app/Contents/MacOS/kicad  (or a nested app inside Contents/Applications)
    wxString path = exe.GetFullPath();
    int      pos = path.Find( wxS( "KiCad.app/Contents" ) );
    wxString contents = pos != wxNOT_FOUND ? path.Left( pos ) + wxS( "KiCad.app/Contents" )
                                           : wxString( wxS( "/Applications/KiCad/KiCad.app/Contents" ) );

    return contents + wxS( "/Frameworks/Python.framework/Versions/Current/bin/python3" );
#elif defined( __WXMSW__ )
    wxString found = firstExisting( { exe.GetPath() + wxS( "\\python.exe" ) } );
    return found.IsEmpty() ? wxString( wxS( "python.exe" ) ) : found;
#else
    wxString found = firstExisting( { wxS( "/usr/bin/python3" ) } );
    return found.IsEmpty() ? wxString( wxS( "python3" ) ) : found;
#endif
}


static wxString defaultPythonPath()
{
#if !defined( __WXMAC__ ) && !defined( __WXMSW__ )
    // A KiCad installed to <prefix>/bin puts pcbnew.py in <prefix>/lib/python3/dist-packages.
    wxFileName exe( wxStandardPaths::Get().GetExecutablePath() );
    wxFileName dir( exe.GetPath(), wxEmptyString );
    dir.RemoveLastDir();
    dir.AppendDir( wxS( "lib" ) );
    dir.AppendDir( wxS( "python3" ) );
    dir.AppendDir( wxS( "dist-packages" ) );

    if( wxFileName::FileExists( dir.GetPath() + wxS( "/pcbnew.py" ) ) )
        return dir.GetPath();
#endif
    return wxEmptyString;
}


/**
 * Folder holding the components shipped inside the KiCad-Claude package, if any:
 *   macOS:   KiCad.app/Contents/SharedSupport/claude
 *   Windows: <install>\\claude   (next to <install>\\bin)
 *   Linux:   <prefix>/share/kicad-claude
 */
static wxString bundledDir()
{
    wxFileName exe( wxStandardPaths::Get().GetExecutablePath() );
    wxString   candidate;

#if defined( __WXMAC__ )
    wxString path = exe.GetFullPath();
    int      pos = path.Find( wxS( ".app/Contents/" ) );

    if( pos != wxNOT_FOUND )
        candidate = path.Left( pos ) + wxS( ".app/Contents/SharedSupport/claude" );
#elif defined( __WXMSW__ )
    wxFileName dir( exe.GetPath(), wxEmptyString );
    dir.RemoveLastDir();
    candidate = dir.GetPath() + wxS( "\\claude" );
#else
    wxFileName dir( exe.GetPath(), wxEmptyString );
    dir.RemoveLastDir();
    candidate = dir.GetPath() + wxS( "/share/kicad-claude" );
#endif

    return !candidate.IsEmpty() && wxFileName::DirExists( candidate ) ? candidate : wxString();
}


CLAUDE_CHAT_CONFIG CLAUDE_CHAT_CONFIG::Load()
{
    CLAUDE_CHAT_CONFIG cfg;
    wxFFile            file;

    if( wxFileName::FileExists( ConfigFilePath() ) && file.Open( ConfigFilePath(), wxS( "r" ) ) )
    {
        wxString text;

        if( file.ReadAll( &text, wxConvUTF8 ) )
        {
            try
            {
                nlohmann::json j = nlohmann::json::parse( text.ToStdString( wxConvUTF8 ) );

                auto get = [&]( const char* aName, wxString& aOut )
                {
                    if( j.contains( aName ) && j[aName].is_string() )
                        aOut = wxString::FromUTF8( j[aName].get<std::string>() );
                };

                get( "model", cfg.m_model );
                get( "node", cfg.m_node );
                get( "server", cfg.m_server );
                get( "kicad_python", cfg.m_kicadPython );
                get( "pythonpath", cfg.m_pythonPath );
                get( "toolboxes", cfg.m_toolboxes );
            }
            catch( ... )
            {
                // Malformed file: fall back to defaults
            }
        }
    }

    wxString base = InstallBaseDir();
    wxString sep = wxFileName::GetPathSeparator();
    wxString bundle = bundledDir();

    // Prefer the components shipped inside the package.
    if( !bundle.IsEmpty() )
    {
#ifdef __WXMSW__
        wxString node = bundle + wxS( "\\node\\node.exe" );
#else
        wxString node = bundle + wxS( "/node/bin/node" );
#endif
        wxString server = bundle + sep + wxS( "mcp-server" ) + sep + wxS( "dist" ) + sep
                          + wxS( "index.js" );
        wxString pydeps = bundle + sep + wxS( "pydeps" );

        if( cfg.m_node.IsEmpty() && wxFileName::FileExists( node ) )
            cfg.m_node = node;

        if( cfg.m_server.IsEmpty() && wxFileName::FileExists( server ) )
            cfg.m_server = server;

        if( cfg.m_pythonPath.IsEmpty() && wxFileName::DirExists( pydeps ) )
            cfg.m_pythonPath = pydeps;
    }

    if( cfg.m_model.IsEmpty() )
        cfg.m_model = wxString::FromUTF8( DEFAULT_MODEL );

    if( cfg.m_node.IsEmpty() )
        cfg.m_node = defaultNode( base );

    if( cfg.m_server.IsEmpty() )
        cfg.m_server = base + sep + wxS( "mcp-server" ) + sep + wxS( "dist" ) + sep + wxS( "index.js" );

    if( cfg.m_kicadPython.IsEmpty() )
        cfg.m_kicadPython = defaultKicadPython();

    if( cfg.m_pythonPath.IsEmpty() )
        cfg.m_pythonPath = defaultPythonPath();

    return cfg;
}


bool CLAUDE_CHAT_CONFIG::Save() const
{
    nlohmann::json j;
    j["model"] = m_model.ToStdString( wxConvUTF8 );
    j["node"] = m_node.ToStdString( wxConvUTF8 );
    j["server"] = m_server.ToStdString( wxConvUTF8 );
    j["kicad_python"] = m_kicadPython.ToStdString( wxConvUTF8 );
    j["pythonpath"] = m_pythonPath.ToStdString( wxConvUTF8 );
    j["toolboxes"] = m_toolboxes.ToStdString( wxConvUTF8 );

    wxFFile file( ConfigFilePath(), wxS( "w" ) );

    if( !file.IsOpened() )
        return false;

    return file.Write( wxString::FromUTF8( j.dump( 2 ) ), wxConvUTF8 );
}


bool CLAUDE_CHAT_CONFIG::GetApiKey( wxString& aKey )
{
    wxString env;

    // Developer convenience: an exported ANTHROPIC_API_KEY wins over the stored one.
    if( wxGetEnv( wxS( "ANTHROPIC_API_KEY" ), &env ) && !env.IsEmpty() )
    {
        aKey = env;
        return true;
    }

    return KIPLATFORM::SECRETS::GetSecret( SECRET_SERVICE, SECRET_KEY, aKey ) && !aKey.IsEmpty();
}


bool CLAUDE_CHAT_CONFIG::SetApiKey( const wxString& aKey )
{
    return KIPLATFORM::SECRETS::StoreSecret( SECRET_SERVICE, SECRET_KEY, aKey );
}


bool CLAUDE_CHAT_CONFIG::DeleteApiKey()
{
    return KIPLATFORM::SECRETS::DeleteSecret( SECRET_SERVICE, SECRET_KEY );
}
