#include <chrono>
#include <thread>

#include "win_utils.hpp"

#pragma comment(lib, "LDE64x64.lib")

EXTERN_C int LDE( void*, int );

int wmain( int argc, wchar_t** argv )
{
	if ( argc < 3 )
	{
		const auto full_path = std::filesystem::path( argv[ 0 ] );

		printf( "[!] incorrect usage\n[!] format: %ws dll_name window_class", full_path.filename( ).c_str( ) );
		return -1;
	}

	const auto dll_name = argv[ 1 ];
	const auto window_class = argv[ 2 ];

	if ( !std::filesystem::exists( dll_name ) )
	{
		printf( "[!] dll path supplied does not exist" );
		return -1;
	}

	printf( "[~] welcome to spenldid implanter poc\n" );

	// we need to enable the debug privilege
	if ( !impl::enable_privilege( L"SeDebugPrivilege" ) )
		return -1;

	printf( "[~] enabled debug privilege!\n" );

	printf( "[~] waiting for battleye service...\n" );

	auto be_process_id = 0;

	while ( !be_process_id )
	{
		// check if it's been 2 minutes since start, if it's been 2 minutes & process hasn't been found, break
		static const auto cached_time = std::chrono::system_clock::now( );
		const auto current_time_mins = std::chrono::duration_cast< std::chrono::minutes >( std::chrono::system_clock::now( ) - cached_time ).count( );

		if ( current_time_mins >= 2u )
			break;

		be_process_id = impl::get_process_id( L"BEService.exe" );
		std::this_thread::sleep_for( std::chrono::milliseconds( 250 ) );
	}

	if ( !be_process_id )
	{
		printf( "[!] timed out" );
		return -1;
	}

	printf( "[~] found BEService process [%d]\n", be_process_id );

	// open handle to beservice
	impl::uq_handle be_process_handle{ OpenProcess( PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, be_process_id ), &CloseHandle };

	// return value is NULL incase of failure
	if ( !be_process_handle.get( ) )
	{
		LOG_LAST_ERROR( );
		return -1;
	}

	printf( "[~] opened process handle [0x%p]\n", be_process_handle.get( ) );

	// retrieve module data
	const auto be_data = impl::get_module_data( be_process_handle.get( ), L"BEService.exe" );

	// shouldn't be possible
	if ( !be_data.first )
	{
		printf( "[!] failed to find BEService.exe data" );
		return -1;
	}

	printf( "[~] retrieved BEService address [0x%p]\n", be_data.first );

	// get handle to file on disk
	impl::uq_handle be_disk_handle{ CreateFileW( be_data.second.c_str( ), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, INVALID_HANDLE_VALUE ), &CloseHandle };

	// return value is INVALID_HANDLE_VALUE incase of failure
	if ( be_disk_handle.get( ) == INVALID_HANDLE_VALUE )
	{
		LOG_LAST_ERROR( );
		return -1;
	}

	printf( "[~] opened disk handle [0x%p]\n", be_disk_handle.get( ) );

	auto be_disk_buffer = impl::get_file_data( be_disk_handle.get( ), be_data.second );

	// look for an executable section to deploy hook in
	const auto buffer_start = be_disk_buffer.data( );

	// get the NT header
	const auto nt_header = FIND_NT_HEADER( buffer_start );

	// search for an executable section
	const auto section_header = IMAGE_FIRST_SECTION( nt_header );
	const auto section_header_end = section_header + nt_header->FileHeader.NumberOfSections;

	// get section that has IMAGE_SCN_MEM_EXECUTE flag, and no raw data.
	auto executable_section = std::find_if( section_header, section_header_end, [ ]( const auto& section )
											{
												return section.SizeOfRawData != 0 && ( section.Characteristics & IMAGE_SCN_MEM_EXECUTE ) == IMAGE_SCN_MEM_EXECUTE;
											} );

	if ( executable_section == section_header_end )
	{
		printf( "[!] can't find needed section" );
		return -1;
	}

	printf( "[~] found section [%s]\n", reinterpret_cast< const char* >( executable_section->Name + 1 ) );

	// since w10 1607, the limit for maximum path isn't actually MAX_PATH, just assume it is.
	auto dll_path = std::make_unique<wchar_t[ ]>( MAX_PATH );
	GetFullPathNameW( dll_name, MAX_PATH, dll_path.get( ), nullptr );

	printf( "[~] dll path: %ws\n", dll_path.get( ) );

	const auto dll_path_sz = wcslen( dll_path.get( ) ) * 2;

	auto kernel_path = std::make_unique<wchar_t[ ]>( MAX_PATH );
	GetModuleFileNameW( GetModuleHandleW( L"Kernel32.dll" ), kernel_path.get( ), MAX_PATH );

	printf( "[~] kernel32 path: %ws\n", kernel_path.get( ) );

	const auto kernel_path_sz = wcslen( kernel_path.get( ) ) * 2;

	// allocate a buffer in the process to hold our paths
	const auto paths_buffer = VirtualAllocEx( be_process_handle.get( ), nullptr, dll_path_sz + kernel_path_sz + 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE );

	if ( !paths_buffer )
	{
		LOG_LAST_ERROR( );
		return -1;
	}

	printf( "[~] allocated buffer at 0x%p\n", paths_buffer );

	RET_CHK( WriteProcessMemory( be_process_handle.get( ), paths_buffer, dll_path.get( ), dll_path_sz, nullptr ) )

	printf( "[~] wrote dll path successfully!\n" );

	const auto second_path_buffer = reinterpret_cast< uint8_t* >( paths_buffer ) + dll_path_sz + 2;

	RET_CHK( WriteProcessMemory( be_process_handle.get( ), second_path_buffer, kernel_path.get( ), kernel_path_sz, nullptr ) )

	printf( "[~] wrote kernel32 path successfully!\n" );

	uint8_t jmp_stub[ ]
	{
		0x51, 0x48, 0xb9, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
		0xcc, 0xcc, 0x48, 0x87, 0x0c, 0x24, 0xc3
	};

	// DefCon42 is sexy
	uint8_t shell_code[ ]
	{
		0x41, 0x54, 0x41, 0x55, 0x49, 0x89, 0xca, 0x49, 0xbc,
		0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0x4d,
		0x0f, 0xb7, 0x2c, 0x24, 0x4d, 0x85, 0xed, 0x74, 0x1e,
		0x4d, 0x0f, 0xb7, 0x1a, 0x4d, 0x85, 0xdb, 0x74, 0x1f,
		0x4d, 0x39, 0xeb, 0x75, 0x0a, 0x49, 0x83, 0xc2, 0x02,
		0x49, 0x83, 0xc4, 0x02, 0xeb, 0xde, 0x49, 0x83, 0xc2,
		0x02, 0xeb, 0xce, 0x48, 0xb9, 0xcc, 0xcc, 0xcc, 0xcc,
		0xcc, 0xcc, 0xcc, 0xcc, 0x41, 0x5d, 0x41, 0x5c
	};

	const auto kernel_base = GetModuleHandleW( L"Kernelbase.dll" );

	if ( !kernel_base )
	{
		LOG_LAST_ERROR( );
		return -1;
	}

	const auto export_address = reinterpret_cast< uint8_t* >( GetProcAddress( kernel_base, "CreateFileW" ) );

	if ( !export_address )
	{
		LOG_LAST_ERROR( );
		return -1;
	}

	printf( "[~] found CreateFileW [0x%p]\n", export_address );

	printf( "[~] preparing hook...\n" );

	// calculate buffer length based on bytes needed from original
	auto og_len = 0;

	while ( og_len < sizeof( jmp_stub ) )
		og_len += LDE( export_address + og_len, 64 );

	const auto buf_len = og_len + sizeof( shell_code ) + sizeof( jmp_stub );

	std::vector<uint8_t> buf_data{};
	buf_data.resize( buf_len );

	const auto og_data = export_address + og_len;

	// :)
	memcpy( buf_data.data( ), shell_code, sizeof( shell_code ) );
	memcpy( buf_data.data( ) + 0x9, &paths_buffer, 8 );
	memcpy( buf_data.data( ) + 0x3b, &second_path_buffer, 8 );
	memcpy( buf_data.data( ) + sizeof( shell_code ), export_address, og_len );
	memcpy( buf_data.data( ) + sizeof( shell_code ) + og_len, jmp_stub, sizeof( jmp_stub ) );
	memcpy( buf_data.data( ) + sizeof( shell_code ) + og_len + 3, &og_data, 8 );

	const auto deployment_location = ( reinterpret_cast< uint8_t* >( be_data.first ) + executable_section->VirtualAddress + executable_section->Misc.VirtualSize ) - buf_len;

	printf( "[~] deploying hook...\n" );

	DWORD cache = 0;

	RET_CHK( VirtualProtectEx( be_process_handle.get( ), deployment_location, buf_len, PAGE_EXECUTE_READWRITE, &cache ) )
	RET_CHK( WriteProcessMemory( be_process_handle.get( ), deployment_location, buf_data.data( ), buf_len, nullptr ) )
	RET_CHK( VirtualProtectEx( be_process_handle.get( ), deployment_location, buf_len, cache, &cache ) )

	*reinterpret_cast< uint64_t* >( &jmp_stub[ 3 ] ) = reinterpret_cast< uint64_t >( deployment_location );

	RET_CHK( WriteProcessMemory( be_process_handle.get( ), export_address, jmp_stub, sizeof( jmp_stub ), nullptr ) )

	printf( "[~] hook deployed!\n" );

	printf( "[~] waiting for game to open...\n" );

	HWND game_window = nullptr;

	while ( !game_window )
	{
		// check if it's been 2 minutes since start, if it's been 2 minutes & window hasn't been found, break
		static const auto cached_time = std::chrono::system_clock::now( );
		const auto current_time_mins = std::chrono::duration_cast< std::chrono::minutes >( std::chrono::system_clock::now( ) - cached_time ).count( );

		if ( current_time_mins >= 2u )
			break;

		game_window = FindWindowW( window_class, nullptr );
		std::this_thread::sleep_for( std::chrono::milliseconds( 250 ) );
	}

	if ( !game_window )
	{
		printf( "[!] timed out\n" );
		return -1;
	}

	const auto window_thread = GetWindowThreadProcessId( game_window, nullptr );

	if ( !window_thread )
	{
		LOG_LAST_ERROR( );
		return -1;
	}

	printf( "[~] window thread found [0x%lx]\n", window_thread );

	const auto loaded_module = LoadLibraryW( dll_path.get( ) );

	if ( !loaded_module )
	{
		LOG_LAST_ERROR( );
		return -1;
	}

	printf( "[~] loaded module to local process [0x%p]\n", loaded_module );

	const auto window_hook = GetProcAddress( loaded_module, "wnd_hk" );

	if ( !window_hook )
	{
		printf( "[!] can't find needed export in implanted dll, last error: 0x%lx", GetLastError( ) );
		return -1;
	}

	printf( "[~] posting message...\n" );

	// spam the fuck out of the message handler
	for ( auto i = 0; i < 50; i++ )
		PostThreadMessageW( window_thread, 0x5b0, 0, reinterpret_cast< LPARAM >( shell_code ) );

	printf( "[~] dll implanted\n" );

	printf( "[~] splendid implanter out!\n" );

	return 0;
}
