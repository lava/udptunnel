FMT_LIBS := -lfmt
BOOST_LIBS := -lboost_program_options -lboost_system

UDPTUNNEL_LIBS = $(FMT_LIBS) $(BOOST_LIBS)

# STATIC_BUILD_FLAGS = -static
STATIC_BUILD_FLAGS =

BUNDLED_LIBS_FLAGS = -Iaux/linear_ringbuffer/include
# BUNDLED_LIBS_FLAGS = 

udptunnel: udptunnel.cpp
	g++ udptunnel.cpp -o udptunnel $(BUNDLED_LIBS_FLAGS) $(STATIC_BUILD_FLAGS) $(UDPTUNNEL_LIBS)

run:
	./udptunnel --port 34197 --server example.org --server-user john.doe
