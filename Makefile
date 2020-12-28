FMT_LIBS := -lfmt
BOOST_LIBS := -lboost_program_options -lboost_system

UDPTUNNEL_LIBS = $(FMT_LIBS) $(BOOST_LIBS)

# STATIC_BUILD = -static
STATIC_BUILD =

udptunnel: udptunnel.cpp
	g++ udptunnel.cpp -o udptunnel $(STATIC_BUILD) $(UDPTUNNEL_LIBS)

run:
	./udptunnel --port 34197 --server example.org --server-user john.doe
