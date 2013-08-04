AM_CPPFLAGS = -I$(srcdir)/../statesync -I$(srcdir)/../terminal -I$(srcdir)/../network -I$(srcdir)/../crypto -I../protobufs -I$(srcdir)/../util $(TINFO_CFLAGS) $(protobuf_CFLAGS) $(OPENSSL_CFLAGS)
AM_CXXFLAGS = $(WARNING_CXXFLAGS) $(PICKY_CXXFLAGS) $(HARDEN_CFLAGS) $(MISC_CXXFLAGS)
AM_LDFLAGS  = $(HARDEN_LDFLAGS)
LDADD = ../crypto/libmoshcrypto.a ../network/libmoshnetwork.a ../statesync/libmoshstatesync.a ../terminal/libmoshterminal.a ../util/libmoshutil.a ../protobufs/libmoshprotos.a -lm $(TINFO_LIBS) $(protobuf_LIBS) $(OPENSSL_LIBS)

mosh_server_LDADD = $(LDADD) $(LIBUTIL)

pkglib_LIBRARIES = libmoshclient.a
include_HEADERS = embeddedclient.h terminaloverlay.h

bin_PROGRAMS = emdriver

if BUILD_CLIENT
  bin_PROGRAMS += mosh-client
endif

if BUILD_SERVER
  bin_PROGRAMS += mosh-server
endif

libmoshclient_a_SOURCES = embeddedclient.cc embeddedclient.h terminaloverlay.cc terminaloverlay.h

mosh_client_SOURCES = mosh-client.cc stmclient.cc stmclient.h terminaloverlay.cc terminaloverlay.h
mosh_server_SOURCES = mosh-server.cc

emdriver_SOURCES = emdriver.cc
emdriver_LDADD = $(LDADD) libmoshclient.a
