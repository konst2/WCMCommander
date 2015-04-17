/*
 * Part of WCM Commander
 * https://github.com/corporateshark/WCMCommander
 * walcommander@linderdaum.com
 */

#include "file-exec.h"
#include "ncwin.h"
#include "ltext.h"
#include "string-util.h"
#include "panel.h"
#include "strmasks.h"
#include "ext-app.h"

#ifndef _WIN32
#  include <signal.h>
#  include <sys/wait.h>
#  include "ux_util.h"
#else
#	include "w32util.h"
#endif


#define TERMINAL_THREAD_ID 1


void ReturnToDefaultSysDir()
{
#ifdef _WIN32
	wchar_t buf[4096] = L"";

	if ( GetSystemDirectoryW( buf, 4096 ) > 0 )
	{
		SetCurrentDirectoryW( buf );
	}

#else
	chdir( "/" );
#endif
}


enum
{
	CMD_RC_RUN = 999,
	CMD_RC_OPEN_0 = 1000
};

static const int CMD_OPEN_FILE = 1000;
static const int CMD_EXEC_FILE = 1001;


struct AppMenuData
{
	struct Node
	{
		unicode_t* cmd;
		bool terminal;
		Node() : cmd( 0 ), terminal( 0 ) {}
		Node( unicode_t* c, bool t ) : cmd( c ), terminal( t ) {}
	};
	
	ccollect<clPtr<MenuData>> mData;
	ccollect<Node> nodeList;
	MenuData* AppendAppList( AppList* list );
};

MenuData* AppMenuData::AppendAppList( AppList* list )
{
	if ( !list )
	{
		return 0;
	}

	clPtr<MenuData> p = new MenuData();

	for ( int i = 0; i < list->Count(); i++ )
	{
		if ( list->list[i].sub.ptr() )
		{
			MenuData* sub = AppendAppList( list->list[i].sub.ptr() );
			p->AddSub( list->list[i].name.data(), sub );
		}
		else
		{
			p->AddCmd( nodeList.count() + CMD_RC_OPEN_0, list->list[i].name.data() );
			nodeList.append( Node( list->list[i].cmd.data(), list->list[i].terminal ) );
		}
	}

	MenuData* ret = p.ptr();
	mData.append( p );
	return ret;
}


FileExecutor::FileExecutor( NCWin* NCWin, StringWin& editPref, NCHistory& history, TerminalWin_t& terminal )
	: m_NCWin( NCWin )
	, _editPref( editPref )
	, _history( history )
	, _terminal( terminal )
	, _execId( -1 )
{
	_execSN[0] = 0;
}

void FileExecutor::ShowFileContextMenu( cpoint point, PanelWin* Panel )
{
	FSNode* p = Panel->GetCurrent();

	if ( !p || p->IsDir() )
	{
		return;
	}

	clPtr<AppList> appList = GetAppList( Panel->UriOfCurrent().GetUnicode() );

	//if (!appList.data()) return;

	AppMenuData data;
	MenuData mdRes, *md = data.AppendAppList( appList.ptr() );

	if ( !md )
	{
		md = &mdRes;
	}

	if ( p->IsExe() )
	{
		md->AddCmd( CMD_RC_RUN, _LT( "Execute" ) );
	}

	if ( !md->Count() )
	{
		return;
	}

	int ret = DoPopupMenu( 0, m_NCWin, md, point.x, point.y );

	m_NCWin->SetCommandLineFocus();

	if ( ret == CMD_RC_RUN )
	{
		ExecuteFile( Panel );
		return;
	}

	ret -= CMD_RC_OPEN_0;

	if ( ret < 0 || ret >= data.nodeList.count() )
	{
		return;
	}

	StartExecute( data.nodeList[ret].cmd, Panel->GetFS(), Panel->GetPath(), !data.nodeList[ret].terminal );
}

void FileExecutor::ApplyCommand( const std::vector<unicode_t>& cmd, PanelWin* Panel )
{
	clPtr<FSList> list = Panel->GetSelectedList();

	if ( !cmd.data() || !list.ptr() || list->Count() <= 0 )
	{
		return;
	}

	std::vector<FSNode*> nodes = list->GetArray();

	m_NCWin->SetMode( NCWin::TERMINAL );

	for ( auto i = nodes.begin(); i != nodes.end(); i++ )
	{
		FSNode* Node = *i;

		const unicode_t* Name = Node->GetUnicodeName();

		std::vector<unicode_t> Command = MakeCommand( cmd, Name );

		StartExecute( Command.data(), Panel->GetFS(), Panel->GetPath() );
	}
}

const clNCFileAssociation* FileExecutor::FindFileAssociation( const unicode_t* FileName ) const
{
	const auto& Assoc = g_Env.GetFileAssociations();

	for ( const auto& i : Assoc )
	{
		std::vector<unicode_t> Mask = i.GetMask();

		clMultimaskSplitter Splitter( Mask );

		if ( Splitter.CheckAndFetchAllMasks( FileName ) )
		{
			return &i;
		}
	}

	return nullptr;
}

bool FileExecutor::StartFileAssociation( PanelWin* panel, eFileAssociation Mode )
{
	const unicode_t* FileName = panel->GetCurrentFileName();

	const clNCFileAssociation* Assoc = FindFileAssociation( FileName );

	if ( !Assoc )
	{
		return false;
	}

	std::vector<unicode_t> Cmd = MakeCommand( Assoc->Get( Mode ), FileName );

	if ( Cmd.data() && *Cmd.data() )
	{
		StartExecute( Cmd.data(), panel->GetFS(), panel->GetPath(), !Assoc->GetHasTerminal() );
		return true;
	}

	return false;
}

void FileExecutor::ExecuteFileByEnter( PanelWin* Panel, bool Shift )
{
	FSNode* p = Panel->GetCurrent();

	bool cmdChecked = false;
	std::vector<unicode_t> cmd;
	bool terminal = true;
	const unicode_t* pAppName = 0;

	if ( Shift )
	{
		ExecuteDefaultApplication( Panel->UriOfCurrent().GetUnicode() );
		return;
	}

	if ( StartFileAssociation( Panel, eFileAssociation_Execute ) )
	{
		return;
	}

	if ( g_WcmConfig.systemAskOpenExec )
	{
		cmd = GetOpenCommand( Panel->UriOfCurrent().GetUnicode(), &terminal, &pAppName );
		cmdChecked = true;
	}

	if ( p->IsExe() )
	{
#ifndef _WIN32

		if ( g_WcmConfig.systemAskOpenExec && cmd.data() )
		{
			ButtonDataNode bListOpenExec[] = { { "&Open", CMD_OPEN_FILE }, { "&Execute", CMD_EXEC_FILE }, { "&Cancel", CMD_CANCEL }, { 0, 0 } };

			static unicode_t emptyStr[] = { 0 };

			if ( !pAppName )
			{
				pAppName = emptyStr;
			}

			int ret = NCMessageBox( this, "Open",
				carray_cat<char>( "Executable file: ", p->name.GetUtf8(), "\ncan be opened by: ", unicode_to_utf8( pAppName ).data(), "\nExecute or Open?" ).data(),
				false, bListOpenExec );

			if ( ret == CMD_CANCEL )
			{
				return;
			}

			if ( ret == CMD_OPEN_FILE )
			{
				StartExecute( cmd.data(), Panel->GetFS(), Panel->GetPath(), !terminal );
				return;
			}
		}

#endif
		ExecuteFile( Panel );
		return;
	}

	if ( !cmdChecked )
	{
		cmd = GetOpenCommand( Panel->UriOfCurrent().GetUnicode(), &terminal, 0 );
	}

	if ( cmd.data() )
	{
		StartExecute( cmd.data(), Panel->GetFS(), Panel->GetPath(), !terminal );
	}
}

void FileExecutor::ExecuteFile( PanelWin* panel )
{
	FSNode* p = panel->GetCurrent();

	if ( !p || p->IsDir() || !p->IsExe() )
	{
		return;
	}

	FS* fs = panel->GetFS();

	if ( !fs || fs->Type() != FS::SYSTEM )
	{
		NCMessageBox( m_NCWin, _LT( "Run" ), _LT( "Can`t execute file in not system fs" ), true );
		return;
	}

#ifdef _WIN32
	
	static unicode_t w[2] = { '"', 0 };
	StartExecute( carray_cat<unicode_t>( w, panel->UriOfCurrent().GetUnicode(), w ).data(), fs, panel->GetPath() );

#else

	const unicode_t*   fName = p->GetUnicodeName();
	int len = unicode_strlen( fName );
	std::vector<unicode_t> cmd( 2 + len + 1 );
	cmd[0] = '.';
	cmd[1] = '/';
	memcpy( cmd.data() + 2, fName, len * sizeof( unicode_t ) );
	cmd[2 + len] = 0;
	StartExecute( cmd.data(), fs, panel->GetPath() );

#endif
}

void FileExecutor::StartExecute( const unicode_t* cmd, FS* fs, FSPath& path, bool NoTerminal )
{
	SkipSpaces( cmd );

	if ( StartExecute( _editPref.Get(), cmd, fs, path ) )
	{
		_history.Put( cmd );
		m_NCWin->SetMode( NCWin::TERMINAL );
	}

	ReturnToDefaultSysDir();
}

bool FileExecutor::StartExecute( const unicode_t* pref, const unicode_t* cmd, FS* fs, FSPath& path, bool NoTerminal )
{
#ifdef _WIN32

	if ( !_terminal.Execute( m_NCWin, TERMINAL_THREAD_ID, cmd, 0, fs->Uri( path ).GetUnicode() ) )
	{
		return false;
	}

#else

	static unicode_t empty[] = {0};
	static unicode_t newLine[] = { '\n', 0 };
	
	if ( !pref )
	{
		pref = empty;
	}

	if ( !*cmd )
	{
		return false;
	}

	_terminal.TerminalReset();

	if ( NoTerminal )
	{
		unsigned fg = 0xB;
		unsigned bg = 0;
		
		_terminal.TerminalPrint( newLine, fg, bg );
		_terminal.TerminalPrint( pref, fg, bg );
		_terminal.TerminalPrint( cmd, fg, bg );
		_terminal.TerminalPrint( newLine, fg, bg );

		char* dir = 0;

		if ( fs && fs->Type() == FS::SYSTEM )
		{
			dir = (char*) path.GetString( sys_charset_id );
		}

		FSString s = cmd;
		sys_char_t* SysCmd = (sys_char_t*) s.Get( sys_charset_id );

		pid_t pid = fork();
		if ( pid < 0 )
		{
			return false;
		}

		if ( pid )
		{
			waitpid( pid, 0, 0 );
		}
		else
		{
			if ( !fork() )
			{
				//printf("exec: %s\n", SysCmd);
				signal( SIGINT, SIG_DFL );
				static char shell[] = "/bin/sh";
				const char* params[] = { shell, "-c", SysCmd, NULL };

				if ( dir )
				{
					chdir( dir );
				}

				execv( shell, (char**) params );
				exit( 1 );
			}

			exit( 0 );
		}
	}
	else
	{
		unsigned fg_pref = 0xB;
		unsigned fg_cmd = 0xF;
		unsigned bg = 0;
		
		_terminal.TerminalPrint( newLine, fg_pref, bg );
		_terminal.TerminalPrint( pref, fg_pref, bg );
		_terminal.TerminalPrint( cmd, fg_cmd, bg );
		_terminal.TerminalPrint( newLine, fg_cmd, bg );

		int l = unicode_strlen( cmd );
		int i;

		if ( l >= 64 )
		{
			for ( i = 0; i < 64 - 1; i++ )
			{
				_execSN[i] = cmd[i];
			}

			_execSN[60] = '.';
			_execSN[61] = '.';
			_execSN[62] = '.';
			_execSN[63] = 0;
		}
		else
		{
			for ( i = 0; i < l; i++ )
			{
				_execSN[i] = cmd[i];
			}

			_execSN[l] = 0;
		}

		_terminal.Execute( m_NCWin, TERMINAL_THREAD_ID, cmd, (sys_char_t*) path.GetString( sys_charset_id ) );
	}

#endif

	return true;
}

void FileExecutor::StopExecute()
{
#ifdef _WIN32

	if ( NCMessageBox( m_NCWin, _LT( "Stop" ), _LT( "Drop current console?" ), false, bListOkCancel ) == CMD_OK )
	{
		_terminal.DropConsole();
	}

#else

	if ( _execId > 0 )
	{
		int ret = KillCmdDialog( m_NCWin, _execSN );

		if ( _execId > 0 )
		{
			if ( ret == CMD_KILL_9 )
			{
				kill( _execId, SIGKILL );
			}
			else if ( ret == CMD_KILL )
			{
				kill( _execId, SIGTERM );
			}
		}
	}

#endif
}

void FileExecutor::ThreadSignal( int id, int data )
{
	if ( id == TERMINAL_THREAD_ID )
	{
		_execId = data;
	}
}

void FileExecutor::ThreadStopped( int id, void* data )
{
	if ( id == TERMINAL_THREAD_ID )
	{
		_execId = -1;
		_execSN[0] = 0;
	}
}
