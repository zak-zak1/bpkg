// vim: nospell
#define _GNU_SOURCE

#include <fcntl.h>
#include <stdlib.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "common/null-stream.h"

#include "checksums.h"
#include "error.h"
#include "files.h"
#include "pipe-fork-exec.h"
#include "rules.h"
#include "bpkg-ar.h"
#include "process-control.h"
#include "process-data.h"
#include "write-control.h"


struct fd fds[ PIPES_MAX ];

// Information about the resulting package which is used
// to populate the package index.
struct package_data stats = {
	0,  // Size of files in package
	0,  // Size of the packaged archive
	"", // First checksum
	""  // Second checksum
};

// TODO: Move this to its own file.
void process_output( char * output )
{
	char * const chk1cmd[] = { CHK1_COMMAND, NULL };
	char * const chk2cmd[] = { CHK2_COMMAND, NULL };

	char buffer[4096];

	ssize_t b_read;

	if ( fd(CURRENT_FILE) != -1 )
	{
		// TODO: Error handling
	}

	fd(CURRENT_FILE) = open( output, O_RDONLY );

	if ( fd(CURRENT_FILE) == -1 )
	{
		// TODO: Error handling
	}

	// TODO: fork to sha256sum and sha512sum
	create_pipes( pipe(CHK1_INPUT_R) );
	create_pipes( pipe(CHK1_OUTPUT_R) );
	create_pipes( pipe(CHK2_INPUT_R) );
	create_pipes( pipe(CHK2_OUTPUT_R) );

	pipe_fork_exec( chk1cmd, pipe(CHK1_INPUT_R), pipe(CHK1_OUTPUT_W) );
	pipe_fork_exec( chk2cmd, pipe(CHK2_INPUT_R), pipe(CHK2_OUTPUT_W) );

	while ( 1 )
	{
		b_read = read( fd(CURRENT_FILE), buffer, 4096 );

		if ( b_read == 0 )
		{
			break;
		}

		if ( b_read == -1 )
		{
			perror( "Error reading ar-stream output file" );
			c_exit( 1 );
		}

		stats.packed_size += b_read;

		write( fd(CHK1_INPUT_W), buffer, b_read );
		write( fd(CHK2_INPUT_W), buffer, b_read );
	}

	close_fd( pipe(CHK1_INPUT_W) );
	close_fd( pipe(CHK2_INPUT_W) );

	read( fd(CHK1_OUTPUT_R), stats.chk1, CHK1_SIZE );
	read( fd(CHK2_OUTPUT_R), stats.chk2, CHK2_SIZE );

	close_fd( pipe(CHK1_OUTPUT_R) );
	close_fd( pipe(CHK2_OUTPUT_R) );
}

int main( int argc, char ** argv )
{
	char * arstream[]  = {  "ar-stream", NULL };
	char * tarstream[] = { "tar-stream", NULL, NULL };
	char * const xz[]  = { "xz", NULL };
	char * debdir;

	if ( argc != 3 )
	{
		fprintf( stderr, "Usage: %s [directory] [target.deb]\n", argv[0] );
		return 1;
	}

	for ( size_t i = 0; i < PIPES_MAX; ++i )
	{
		fd(i) = -1;
	}

	fd(SOURCE_DIR) = open( argv[1], O_DIRECTORY | O_RDONLY );

	if ( fd(SOURCE_DIR) == -1 )
	{
		err( 1, "Can not open sourcedir %s: %s\n", argv[1] );
	}

	if ( faccessat( fd(SOURCE_DIR), "debian", X_OK | R_OK, AT_EACCESS ) == -1 )
	{
		// TODO: All of these should call err.
		perror( "Unable to read or execute debian dir" );
		c_exit( 1 );
	}

	if ( faccessat( fd(SOURCE_DIR), "debian/control", R_OK, AT_EACCESS ) == -1 )
	{
		perror( "Unable to read debian/control" );
		c_exit( 1 );
	}

	if ( faccessat( fd(SOURCE_DIR), "manifest", R_OK, AT_EACCESS ) == -1 )
	{
		perror( "Unable to read manifest" );
		c_exit( 1 );
	}

	load_rules();

	// Put the debian path in a string
	debdir = calloc( strlen( argv[1] ) + 10, sizeof(char) );
	sprintf( debdir, "%s/debian", argv[1] );

	// Create the output (.deb) file
	create_file( argv[2], pipe(DEB_OUTPUT) );
	// Create a named pipe for the 'ar' program
	create_fifo( pipe(AR_INPUT_R) );

	// Spawn the ar(1)-like archiver
	pipe_fork_exec( arstream, pipe(AR_INPUT_R), pipe(DEB_OUTPUT) );

	// Write out the Manifest file
	ar_header( "debian-binary" );
		write( fd(AR_INPUT_W), "2.0\n", 4 );
	ar_footer();

	// Write out the control section
	ar_header( "control.tar.xz" );

		// Create pipes $0 | tarstream | xzip
		create_pipes( pipe(TARSTREAM_INPUT_R) );
		create_pipes( pipe(XZIP_INPUT_R) );

		// Fork to tar-stream
		tarstream[1] = debdir;
		pipe_fork_exec( tarstream, pipe(TARSTREAM_INPUT_R), pipe(XZIP_INPUT_W) );

		// Fork to xzip
		pipe_fork_exec( xz, pipe(XZIP_INPUT_R), pipe(AR_INPUT_W) );

		// Write out the data
		stats.installed_size += process_control();

	// End control section
	ar_footer();

	// Write out the data section
	ar_header( "data.tar.xz" );

		// Create pipes $0 | tarstream | xzip
		create_pipes( pipe(TARSTREAM_INPUT_R) );
		create_pipes( pipe(XZIP_INPUT_R) );

		// Fork to tar-stream
		tarstream[1] = argv[1];
		pipe_fork_exec( tarstream, pipe(TARSTREAM_INPUT_R), pipe(XZIP_INPUT_W) );

		// Fork to xzip
		pipe_fork_exec( xz, pipe(XZIP_INPUT_R), pipe(AR_INPUT_W) );

		// Write out the data
		stats.installed_size += process_data();

	// End data sections
	ar_footer();

	ar_header( "" );
	ar_footer();

	while ( wait( NULL ) != -1 );

	process_output( argv[2] );

	write_control( argv[2], stats );

	close_descriptors();

	free( debdir );

	c_exit( 0 );
}
