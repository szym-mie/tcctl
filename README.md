# tcctl

### temperature control control (I mangled it big time)

### for RaspberryPI

## what is that

it's a deamon for switching the fan on and off. but this description doesnt't do it justice. it's a complete system, capable of remote control, with a handful of configuration options, logging and most importantly automatic fan operation.

every config entry can be reloaded on the go, every event is logged and a lot of most used operations can be triggered via a convienent interface (it's on the way).

for remote access the idea is to take advantage of ssh and scp. while this does not provide any easy way for creating APIs, it does not introduce any security vulernabilities to the system.

## features

- robust operation - code is fairly easy to understand if a little too monolithic
- simple, flexible configuration - just take a look a the provided example
- local socket interface for communicating with clients - again, still cooking, but should provide user with most commonly used options and more.
