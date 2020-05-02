FMT_LIBS := -lfmt
BOOST_LIBS := -lboost_program_options -lboost_system

UDPTUNNEL_LIBS = $(FMT_LIBS) $(BOOST_LIBS)

udptunnel: udptunnel.cpp
	g++ udptunnel.cpp -o udptunnel -static $(UDPTUNNEL_LIBS)

run:
	./udptunnel --port 34197 --server example.org --server-user john.doe
