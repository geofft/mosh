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

#include <errno.h>
#include <locale.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <pwd.h>
#include <signal.h>
#include <time.h>

/*
#if HAVE_PTY_H
#include <pty.h>
#elif HAVE_UTIL_H
#include <util.h>
#endif
*/

#include "embeddedclient.h"
#include "swrite.h"
#include "completeterminal.h"
#include "user.h"
#include "fatal_assert.h"
#include "locale_utils.h"
#include "pty_compat.h"
#include "timestamp.h"

#include "networktransport.cc"

void EmbeddedClient::init( void )
{
  /* Add our name to window title */
  if ( !getenv( "MOSH_TITLE_NOPREFIX" ) ) {
    overlays.set_title_prefix( wstring( L"[mosh] " ) );
  }

  wchar_t tmp[ 128 ];
  swprintf( tmp, 128, L"Nothing received from server on UDP port %d.", port.c_str() );
  connecting_notification = wstring( tmp );

  /* local state */
  /* these will be swapped in the first successful call to update_framebuffers */
  local_framebuffer = new Terminal::Framebuffer( 1, 1 );
  new_state = new Terminal::Framebuffer( cols, rows );

  /* open network */
  Network::UserStream blank;
  Terminal::Complete local_terminal( cols, rows );
  network = new Network::Transport< Network::UserStream, Terminal::Complete >( blank, local_terminal,
									       key.c_str(), ip.c_str(), port.c_str() );

  network->set_send_delay( 1 ); /* minimal delay on outgoing keystrokes */

  /* tell server the size of the terminal */
  network->get_current_state().push_back( Parser::Resize( cols, rows ) );
}

void EmbeddedClient::shutdown( void )
{
#if 0
  /* Restore screen state */
  overlays.get_notification_engine().set_notification_string( wstring( L"" ) );
  overlays.get_notification_engine().server_heard( timestamp() );
  overlays.set_title_prefix( wstring( L"" ) );
  output_new_frame();
#endif

  if ( still_connecting() ) {
    fprintf( stderr, "\nmosh did not make a successful connection to %s:%s.\n", ip.c_str(), port.c_str() );
    fprintf( stderr, "Please verify that UDP port %s is not firewalled and can reach the server.\n\n", port.c_str() );
    fprintf( stderr, "(By default, mosh uses a UDP port between 60000 and 61000. The -p option\nselects a specific UDP port number.)\n" );
  } else if ( network ) {
    if ( !clean_shutdown ) {
      fprintf( stderr, "\n\nmosh did not shut down cleanly. Please note that the\nmosh-server process may still be running on the server.\n" );
    }
  }
}

bool EmbeddedClient::update_framebuffers( void )
{
  if ( !network ) { /* clean shutdown even when not initialized */
    return false;
  }

  /* switch pointers */
  Terminal::Framebuffer *tmp = new_state;
  new_state = local_framebuffer;
  local_framebuffer = tmp;

  /* fetch target state */
  *new_state = network->get_latest_remote_state().state.get_fb();

  /* apply local overlays */
  overlays.apply( *new_state );

  return true;
}

void EmbeddedClient::process_network_input( void )
{
  network->recv();
  
  /* Now give hints to the overlays */
  overlays.get_notification_engine().server_heard( network->get_latest_remote_state().timestamp );
  overlays.get_notification_engine().server_acked( network->get_sent_state_acked_timestamp() );

  overlays.get_prediction_engine().set_local_frame_acked( network->get_sent_state_acked() );
  overlays.get_prediction_engine().set_send_interval( network->send_interval() );
  overlays.get_prediction_engine().set_local_frame_late_acked( network->get_latest_remote_state().state.get_echo_ack() );
}

bool EmbeddedClient::process_user_input( int fd )
{
  const int buf_size = 16384;
  char buf[ buf_size ];

  /* fill buffer if possible */
  ssize_t bytes_read = read( fd, buf, buf_size );
  if ( bytes_read == 0 ) { /* EOF */
    return false;
  } else if ( bytes_read < 0 ) {
    perror( "read" );
    return false;
  }

  if ( !network->shutdown_in_progress() ) {
    overlays.get_prediction_engine().set_local_frame_sent( network->get_sent_state_last() );

    for ( int i = 0; i < bytes_read; i++ ) {
      char the_byte = buf[ i ];

      overlays.get_prediction_engine().new_user_byte( the_byte, *local_framebuffer );

      const static wstring help_message( L"Commands: Ctrl-Z suspends, \".\" quits, \"^\" gives literal Ctrl-^" );

      if ( quit_sequence_started ) {
	if ( the_byte == '.' ) { /* Quit sequence is Ctrl-^ . */
	  if ( network->has_remote_addr() && (!network->shutdown_in_progress()) ) {
	    overlays.get_notification_engine().set_notification_string( wstring( L"Exiting on user request..." ), true );
	    network->start_shutdown();
	    return true;
	  } else {
	    return false;
	  }
	} else if ( the_byte == '^' ) {
	  /* Emulation sequence to type Ctrl-^ is Ctrl-^ ^ */
	  network->get_current_state().push_back( Parser::UserByte( 0x1E ) );
	} else {
	  /* Ctrl-^ followed by anything other than . and ^ gets sent literally */
	  network->get_current_state().push_back( Parser::UserByte( 0x1E ) );
	  network->get_current_state().push_back( Parser::UserByte( the_byte ) );	  
	}

	quit_sequence_started = false;

	if ( overlays.get_notification_engine().get_notification_string() == help_message ) {
	  overlays.get_notification_engine().set_notification_string( L"" );
	}

	continue;
      }

      quit_sequence_started = (the_byte == 0x1E);
      if ( quit_sequence_started ) {
	overlays.get_notification_engine().set_notification_string( help_message, true, false );
	continue;
      }

      network->get_current_state().push_back( Parser::UserByte( the_byte ) );		
    }
  }

  return true;
}

int EmbeddedClient::wait_time( void ) const
{
      int wait_time = min( network->wait_time(), overlays.wait_time() );

      /* Handle startup "Connecting..." message */
      if ( still_connecting() ) {
	wait_time = min( 250, wait_time );
      }

      return wait_time;
}

bool EmbeddedClient::start_shutdown( bool signal )
{
  wstring notification( signal ? L"Signal received, shutting down..." : L"Exiting..." );

  if ( !network->has_remote_addr() ) {
    return true;
  } else if ( !network->shutdown_in_progress() ) {
    overlays.get_notification_engine().set_notification_string( notification, true );
    network->start_shutdown();
  }

  return false;
}

bool EmbeddedClient::tick( void )
{
  /* quit if our shutdown has been acknowledged */
  if ( network->shutdown_in_progress() && network->shutdown_acknowledged() ) {
    clean_shutdown = true;
    return true;
  }

  /* quit after shutdown acknowledgement timeout */
  if ( network->shutdown_in_progress() && network->shutdown_ack_timed_out() ) {
    return true;
  }

  /* quit if we received and acknowledged a shutdown request */
  if ( network->counterparty_shutdown_ack_sent() ) {
    clean_shutdown = true;
    return true;
  }

  /* write diagnostic message if can't reach server */
  if ( still_connecting()
       && (!network->shutdown_in_progress())
       && (timestamp() - network->get_latest_remote_state().timestamp > 250) ) {
    if ( timestamp() - network->get_latest_remote_state().timestamp > 15000 ) {
      if ( !network->shutdown_in_progress() ) {
	overlays.get_notification_engine().set_notification_string( wstring( L"Timed out waiting for server..." ), true );
	network->start_shutdown();
      }
    } else {
      overlays.get_notification_engine().set_notification_string( connecting_notification );
    }
  } else if ( (network->get_remote_state_num() != 0)
	      && (overlays.get_notification_engine().get_notification_string()
		  == connecting_notification) ) {
    overlays.get_notification_engine().set_notification_string( L"" );
  }

  network->tick();

  const Network::NetworkException *exn = network->get_send_exception();
  if ( exn ) {
    overlays.get_notification_engine().set_network_exception( *exn );
  } else {
    overlays.get_notification_engine().clear_network_exception();
  }

  return false;
}

bool EmbeddedClient::handle_exception( const std::exception &e ) {
  const Network::NetworkException *ne;
  const Crypto::CryptoException *ce;
  if ( ne = dynamic_cast<const Network::NetworkException *>( &e ) ) {
    if ( !network->shutdown_in_progress() ) {
      overlays.get_notification_engine().set_network_exception( e );
    }

    struct timespec req;
    req.tv_sec = 0;
    req.tv_nsec = 200000000; /* 0.2 sec */
    nanosleep( &req, NULL );
    freeze_timestamp();

    return true;
  } else if ( ce = dynamic_cast<const Crypto::CryptoException *>( &e ) ) {
    if ( ce->fatal ) {
      return false;
    } else {
      wchar_t tmp[ 128 ];
      swprintf( tmp, 128, L"Crypto exception: %s", ce->what() );
      overlays.get_notification_engine().set_notification_string( wstring( tmp ) );
    }

    return true;
  }

  return false;
}
