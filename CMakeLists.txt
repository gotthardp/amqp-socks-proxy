PROJECT(amqp_proxy C)
#SET(CMAKE_VERBOSE_MAKEFILE ON)

CMAKE_MINIMUM_REQUIRED(VERSION 2.8)

SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pedantic -Wall -Wextra")

ADD_EXECUTABLE(amqp_proxy
	amqp_proxy.c
	amqp_filter.c)

INSTALL(TARGETS amqp_proxy
	RUNTIME DESTINATION bin)

# end of file
