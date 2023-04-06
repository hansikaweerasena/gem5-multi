
# Multicast Development Notes

Richard Bachmann



## 2023-04-02 -- se.py has been deprecated

I switched to the `develop` branch (as recommended by the gem5 contribution docs),
compiled it, and tried running a test simulation using se.py.
I got the following error:
```
fatal: The 'configs/example/se.py' script has been deprecated.
It can be found in 'configs/deprecated/example' if required.
Its usage should be avoided as it will be removed in future releases of gem5.
```
I need to know why se.py was deprecated and what the recommended alternative is.

Using git-blame and git-show, I found the commit where this was changed.
Here is a link to the code review and discussion:
https://gem5-review.googlesource.com/c/public/gem5/+/68157 .

There is not much info here, but the conversation indicates
the gem5 developers really want people to stop using se.py and fs.py.

More information:
https://gem5.atlassian.net/browse/GEM5-1278

> This whole process is a big thumbs-up from me.
> These scripts are causing us a lot of headaches and have become difficult to maintain.
> The gem5 stdlib should be able to meet the vast majority of se/fs.py use-cases come the next release.

## 2023-04-02 -- gem5 stdlib

https://www.gem5.org/documentation/gem5-stdlib/overview

The gem5 standard library was introduced in 2021.
It looks like it is intended to be an easy way to set up gem5 simulations in python.

This is neat:

> A core feature of the gem5 stdlib resource package is that
> it allows users to automatically obtain prebuilt gem5 resources for their simulation.
> A user may specify in their Python config file that a specific gem5 resource is required and,
> when run, the package will check if there is a local copy on the host system,
> and if not, download it.



## 2023-04-04

### Add option to enable multicast

Before actually adding multicast support,
we need a way for the user to choose between running a simulation with and without multicast support.
This is important for testing.

Doing so will probably require seeing how the gem5 stdlib does it.
This looks useful:
https://www.gem5.org/documentation/gem5-stdlib/develop-own-components-tutorial

> The gem5 standard library code resides in src/python/gem5

### Garnet and gem5 stdlib

How does garnet work with gem5 stdlib?
I haven't tried using garnet with the stdlib yet.

I looked through `src/python/gem5` and I couldn't find any indication of garnet support.
The developers probably haven't gotten to it yet.

This isn't a big problem, because we can still use se.py to set up the simulations
(it's just been moved to a 'deprecated' directory).
I'll need to look at se.py to try and figure out how to add a way to toggle-on multicast.
While I'm doing that, I may find it would be easy to add stdlib support for garnet myself,
according to the tutorial.

### Examining se.py

se.py doesn't directly handle garnet options.

Here are some relevant lines:
```
addToPath("../../")

from ruby import Ruby

from common import Options
```
and
```
parser = argparse.ArgumentParser()
Options.addCommonOptions(parser)
Options.addSEOptions(parser)

if "--ruby" in sys.argv:
    Ruby.define_options(parser)
```

### Examining configs/ruby/Ruby.py

In `def create_system`, it looks like the garnet configuration is passed to
configs/network/Network.py and configs/topologies/.

We aren't making any topology modifications, so I'll just look at Network.py.

### Examining configs/network/Network.py

A lot of the NoC-specific options are defined here.

`def create_network` and `init_network` look especially relevant.

There is a 'NetworkClass' class defined in some other file.

It carries some important info about the network and seems to be passed to
`init_network` at some point.
It might be a good place to add the multicast toggle.

Wait, NetworkClass is not a class, it is a variable that is assigned either
'GarnetNetwork' or 'SimpleNetwork'.

From grep:
```
src/mem/ruby/network/garnet/GarnetNetwork.py:class GarnetNetwork(RubyNetwork):
```

### Examining src/mem/ruby/network/garnet/GarnetNetwork.py

I'm out of the configs and into the src now.
I need to figure out how to set a C++ boolean (for the multicast toggle) with python.

It looks like this is being done with a `Param` object.
I think Hansika mentioned this before.

### Adding new params:

How do I add new variable to Param? Solution:
https://www.gem5.org/documentation/learning_gem5/part2/parameters/

On the python side:
```
time_to_wait = Param.Latency("Time before firing the event")
```

On the c++ side:
```
HelloObject::HelloObject(const HelloObjectParams &params) :
    SimObject(params),
    event(*this),
    myName(params.name),
    latency(params.time_to_wait),
    timesLeft(params.number_of_fires)
{
    DPRINTF(Hello, "Created the hello object with the name %s\n", myName);
}
```
I've seen this kind of code in the garnet src before.

### se.py or stdlib?

In https://gem5.atlassian.net/browse/GEM5-1278
(noted earlier) it says:

> This is a stepping stone to remove fs.py and se.py and all (or most) of the option library from gem5

The gem5 developers do not like the current strategy of command-line options being
passed around to set the params.
However, trying to rewrite all of this configuration stuff into gem5 stdlib looks
time consuming and is a yet another detour from the detour we are currently on.
I think it would be best to just add another option and continue using se.py.



## 2023-04-05

Where in the garnet src files should the multicast toggle param be added?

In src/mem/ruby/network/garnet/GarnetNetwork.cc,
the GarnetNetwork constructor accepts many params,
including 'routing_algorithm' and 'ni_flit_size'.
It seems reasonable to have it handle the multicast toggle.

### Adding the multicast toggle param

Steps:
- Register the command line option.
- Make sure the option is passed to the correct param-setting function.
- Register a new param.
- Read the param in the GarnetNetwork constructor.
- Print a debug message indicating the option was or was not set.

### Registering the flag

I added a --multicast flag to my simulation command.
The command failed when run (as it should).

I added the following to configs/network/Network.py:
```
    parser.add_argument(
        "--multicast",
        action="store_true",
        default=False,
        help="Enable multicast routing. Default is multiple-unicast.",
    )
```
The simulation no longer fails and, when you append --help to the command,
it prints out this message:
```
  --multicast           Enable multicast routing. Default is multiple-unicast.
```

### Registering the param

In src/mem/ruby/network/garnet/GarnetNetwork.py, class GarnetNetwork, I added:
```
    enable_multicast = Param.Bool(False, "enable multicast routing")
```
And in configs/network/Network.py, def init_network, I added:
```
        network.enable_multicast = options.multicast
```
When I ran the simulation, I got this error:
```
AttributeError: Class GarnetNetwork has no parameter enable_multicast
```
Even though I was only editing python files, I think I need to recompile gem5.
```
scons ./build/X86/gem5.opt -j 6
```
Error:
```
scons: *** [build/X86/systemc/tlm_core/2/quantum/global_quantum.o]
TypeError : File /home/rb/uf/research/gem5-multicast/src/systemc/ext/systemc found
where directory expected.
scons: building terminated because of errors.
```
Solution: delete build folder and recompile.
https://stackoverflow.com/questions/60520776/error-when-building-gem5-typeerror-file-hdd-me-gem5-src-systemc-ext-systemc?rq=2

After recompiling, the se-hello.sh script runs.
There is still no change in output at this point.



## 2023-04-06

### Connecting the --multicast flag to GarnetNetwork.cc

The python changes are done.
Now I need to add the param to the GarnetNetwork constructor in GarnetNetwork.cc.
I also need to add a variable in GarnetNetwork.hh.

In src/mem/ruby/network/garnet/GarnetNetwork.hh, in the 'protected' category, I add:
```
    bool m_enable_multicast;
```
All of the member variables in the class are prefixed with 'm_'.
I'll follow the convention.

In src/mem/ruby/network/garnet/GarnetNetwork.cc, in the constructor, I add:
```
    m_enable_multicast = p.enable_multicast;
```

To test this, I'll add a debug print statement.
gem5 had a macro for this called DPRINTF:
https://www.gem5.org/documentation/learning_gem5/part2/debugging/

Out of all of the available debug flags,
'RubyNetwork' is probably the most appropriate.
It might be worth adding a 'multicast' debug flag later though.

Here's what I added:
```
    if (m_enable_multicast)
        DPRINTF(RubyNetwork, "Multicast enabled.\n");
    else
        DPRINTF(RubyNetwork, "Multicast not enabled."
            " Using multiple-unicast instead.\n");
```

I recompile gem5 and add `--debug-flags=RubyNetwork` to se-hello.sh.

It produces a ton of output, but towards the top is the following line:
```
      0: system.ruby.network: Multicast enabled.
```

To add a new debug flag, I add this to the 'SConscript' file in the same directory:
```
DebugFlag('GarnetMulticast')
```
And I change the DPRINTFs to use the new flag.

I also needed to add this to GarnetNetwork.cc:
```
#include "debug/GarnetMulticast.hh"
```

After recompiling and changing se-hello.sh to use `--debug-flags=GarnetMulticast`,
far less debug output is produced, with just the desired line.

Removing the `--multicast` flag produces:
```
      0: system.ruby.network: Multicast not enabled. Using multiple-unicast instead.
```

The multicast toggle is now installed.

### Next steps

Hansika and I already decided on a flit encoding strategy (destination bitvector).
I need to figure out how many flits each packet will consist of.
I also need to figure out which routing algorithm to implement.
