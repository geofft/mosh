/*
    Mosh: the mobile shell
    Copyright 2012 Keith Winstein

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give
    permission to link the code of portions of this program with the
    OpenSSL library under certain conditions as described in each
    individual source file, and distribute linked combinations including
    the two.

    You must obey the GNU General Public License in all respects for all
    of the code used other than OpenSSL. If you modify file(s) with this
    exception, you may extend this exception to your version of the
    file(s), but you are not obligated to do so. If you do not wish to do
    so, delete this exception statement from your version. If you delete
    this exception statement from all source files in the program, then
    also delete it here.
*/

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include "embeddedclient.h"
#include "crypto.h"
#include "locale_utils.h"
#include "fatal_assert.h"

void usage( const char *argv0 ) {
  fprintf( stderr, "mosh-client (%s)\n", PACKAGE_STRING );
  fprintf( stderr, "Copyright 2012 Keith Winstein <mosh-devel@mit.edu>\n" );
  fprintf( stderr, "License GPLv3+: GNU GPL version 3 or later <http://gnu.org/licenses/gpl.html>.\nThis is free software: you are free to change and redistribute it.\nThere is NO WARRANTY, to the extent permitted by law.\n\n" );

  fprintf( stderr, "Usage: %s IP PORT\n       %s -c\n", argv0, argv0 );
}

int main( int argc, char *argv[] )
{
  /* For security, make sure we don't dump core */
  Crypto::disable_dumping_core();

  /* Detect edge case */
  fatal_assert( argc > 0 );

  char *ip, *desired_port;

  if ( argc != 3 ) {
    usage( argv[ 0 ] );
    exit( 1 );
  }

  ip = argv[ optind ];
  desired_port = argv[ optind + 1 ];

  /* Sanity-check arguments */
  if ( desired_port
       && ( strspn( desired_port, "0123456789" ) != strlen( desired_port ) ) ) {
    fprintf( stderr, "%s: Bad UDP port (%s)\n\n", argv[ 0 ], desired_port );
    usage( argv[ 0 ] );
    exit( 1 );
  }

  /* Read key from environment */
  char *env_key = getenv( "MOSH_KEY" );
  if ( env_key == NULL ) {
    fprintf( stderr, "MOSH_KEY environment variable not found.\n" );
    exit( 1 );
  }

  /* Read prediction preference */
  char *predict_mode = getenv( "MOSH_PREDICTION_DISPLAY" );
  /* can be NULL */

  char *key = strdup( env_key );
  if ( key == NULL ) {
    perror( "strdup" );
    exit( 1 );
  }

  if ( unsetenv( "MOSH_KEY" ) < 0 ) {
    perror( "unsetenv" );
    exit( 1 );
  }

  try {
    EmbeddedClient client( ip, desired_port, key, predict_mode );
    client.init();

    try {
      client.main();
    } catch ( Network::NetworkException e ) {
      fprintf( stderr, "Network exception: %s: %s\r\n",
	       e.function.c_str(), strerror( e.the_errno ) );
    } catch ( Crypto::CryptoException e ) {
      fprintf( stderr, "Crypto exception: %s\r\n",
	       e.text.c_str() );
    }

    client.shutdown();
  } catch ( const Network::NetworkException& e ) {
    fprintf( stderr, "Network exception: %s: %s\r\n",
	     e.function.c_str(), strerror( e.the_errno ) );
  } catch ( const Crypto::CryptoException& e ) {
    fprintf( stderr, "Crypto exception: %s\r\n",
	     e.text.c_str() );
  } catch ( const std::string& s ) {
    fprintf( stderr, "Error: %s\r\n", s.c_str() );
  }

  printf( "\n[mosh is exiting.]\n" );

  free( key );

  return 0;
}
