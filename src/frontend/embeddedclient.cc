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
#include "select.h"
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
}

void EmbeddedClient::shutdown( void )
{
  /* Restore screen state */
  overlays.get_notification_engine().set_notification_string( wstring( L"" ) );
  overlays.get_notification_engine().server_heard( timestamp() );
  overlays.set_title_prefix( wstring( L"" ) );
  output_new_frame();

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

void EmbeddedClient::main_init()
{
  Select &sel = Select::get_instance();
  sel.add_signal( SIGTERM );
  sel.add_signal( SIGINT );
  sel.add_signal( SIGHUP );
  sel.add_signal( SIGPIPE );

  /* local state */
  local_framebuffer = new Terminal::Framebuffer( cols, rows );
  new_state = new Terminal::Framebuffer( 1, 1 );

  /* open network */
  Network::UserStream blank;
  Terminal::Complete local_terminal( cols, rows );
  network = new Network::Transport< Network::UserStream, Terminal::Complete >( blank, local_terminal,
									       key.c_str(), ip.c_str(), port.c_str() );

  network->set_send_delay( 1 ); /* minimal delay on outgoing keystrokes */

  /* tell server the size of the terminal */
  network->get_current_state().push_back( Parser::Resize( cols, rows ) );
}

void EmbeddedClient::output_new_frame( void )
{
  if ( !network ) { /* clean shutdown even when not initialized */
    return;
  }

  /* fetch target state */
  *new_state = network->get_latest_remote_state().state.get_fb();

  /* apply local overlays */
  overlays.apply( *new_state );

  /* apply any mutations */
  display.downgrade( *new_state );

  /* calculate minimal difference from where we are */
  if ( (local_framebuffer->ds.get_width() != new_state->ds.get_width())
       || (local_framebuffer->ds.get_height() != new_state->ds.get_height()) ) {
    printf( "Changed height to %d, width to %d\n",
	    new_state->ds.get_width(),
	    new_state->ds.get_height() );
  } else {
    for ( int row = 0; row < new_state->ds.get_height(); row++ ) {
      if ( !(*(local_framebuffer->get_row( row )) == *(new_state->get_row( row ))) ) {
	const Terminal::Row *r = new_state->get_row( row );
	printf( "Row %d changed, %zu elements: [", row, r->cells.size() );
	for ( Terminal::Row::cells_type::const_iterator i = r->cells.begin();
	      i != r->cells.end();
	      i++ ) {
	  printf( "%lc", i->debug_contents() );
	}
	printf( " ]\n" );
      }
    }
  }

  repaint_requested = false;

  /* switch pointers */
  Terminal::Framebuffer *tmp = new_state;
  new_state = local_framebuffer;
  local_framebuffer = tmp;
}

bool EmbeddedClient::process_network_input( void )
{
  network->recv();
  
  /* Now give hints to the overlays */
  overlays.get_notification_engine().server_heard( network->get_latest_remote_state().timestamp );
  overlays.get_notification_engine().server_acked( network->get_sent_state_acked_timestamp() );

  overlays.get_prediction_engine().set_local_frame_acked( network->get_sent_state_acked() );
  overlays.get_prediction_engine().set_send_interval( network->send_interval() );
  overlays.get_prediction_engine().set_local_frame_late_acked( network->get_latest_remote_state().state.get_echo_ack() );

  return true;
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

      if ( the_byte == 0x0C ) { /* Ctrl-L */
	repaint_requested = true;
      }

      network->get_current_state().push_back( Parser::UserByte( the_byte ) );		
    }
  }

  return true;
}

void EmbeddedClient::main()
{
  /* initialize signal handling and structures */
  main_init();

  /* prepare to poll for events */
  Select &sel = Select::get_instance();

  while ( 1 ) {
    try {
      output_new_frame();

      int wait_time = min( network->wait_time(), overlays.wait_time() );

      /* Handle startup "Connecting..." message */
      if ( still_connecting() ) {
	wait_time = min( 250, wait_time );
      }

      /* poll for events */
      /* network->fd() can in theory change over time */
      sel.clear_fds();
      std::vector< int > fd_list( network->fds() );
      for ( std::vector< int >::const_iterator it = fd_list.begin();
	    it != fd_list.end();
	    it++ ) {
	sel.add_fd( *it );
      }
      sel.add_fd( STDIN_FILENO );

      int active_fds = sel.select( wait_time );
      if ( active_fds < 0 ) {
	perror( "select" );
	break;
      }

      bool network_ready_to_read = false;

      for ( std::vector< int >::const_iterator it = fd_list.begin();
	    it != fd_list.end();
	    it++ ) {
	if ( sel.read( *it ) ) {
	  /* packet received from the network */
	  /* we only read one socket each run */
	  network_ready_to_read = true;
	}

	if ( sel.error( *it ) ) {
	  /* network problem */
	  break;
	}
      }

      if ( network_ready_to_read ) {
	if ( !process_network_input() ) { return; }
      }
    
      if ( sel.read( STDIN_FILENO ) ) {
	/* input from the user needs to be fed to the network */
	if ( !process_user_input( STDIN_FILENO ) ) {
	  if ( !network->has_remote_addr() ) {
	    break;
	  } else if ( !network->shutdown_in_progress() ) {
	    overlays.get_notification_engine().set_notification_string( wstring( L"Exiting..." ), true );
	    network->start_shutdown();
	  }
	}
      }

      if ( sel.signal( SIGTERM )
           || sel.signal( SIGINT )
           || sel.signal( SIGHUP )
           || sel.signal( SIGPIPE ) ) {
        /* shutdown signal */
        if ( !network->has_remote_addr() ) {
          break;
        } else if ( !network->shutdown_in_progress() ) {
          overlays.get_notification_engine().set_notification_string( wstring( L"Signal received, shutting down..." ), true );
          network->start_shutdown();
        }
      }

      if ( sel.error( STDIN_FILENO ) ) {
	/* user problem */
	if ( !network->has_remote_addr() ) {
	  break;
	} else if ( !network->shutdown_in_progress() ) {
	  overlays.get_notification_engine().set_notification_string( wstring( L"Exiting..." ), true );
	  network->start_shutdown();
	}
      }

      /* quit if our shutdown has been acknowledged */
      if ( network->shutdown_in_progress() && network->shutdown_acknowledged() ) {
	clean_shutdown = true;
	break;
      }

      /* quit after shutdown acknowledgement timeout */
      if ( network->shutdown_in_progress() && network->shutdown_ack_timed_out() ) {
	break;
      }

      /* quit if we received and acknowledged a shutdown request */
      if ( network->counterparty_shutdown_ack_sent() ) {
	clean_shutdown = true;
	break;
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
    } catch ( const Network::NetworkException &e ) {
      if ( !network->shutdown_in_progress() ) {
        overlays.get_notification_engine().set_network_exception( e );
      }

      struct timespec req;
      req.tv_sec = 0;
      req.tv_nsec = 200000000; /* 0.2 sec */
      nanosleep( &req, NULL );
      freeze_timestamp();
    } catch ( const Crypto::CryptoException &e ) {
      if ( e.fatal ) {
        throw;
      } else {
        wchar_t tmp[ 128 ];
        swprintf( tmp, 128, L"Crypto exception: %s", e.text.c_str() );
        overlays.get_notification_engine().set_notification_string( wstring( tmp ) );
      }
    }
  }
}

