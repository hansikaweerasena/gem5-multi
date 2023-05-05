
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



## 2023-04-07

### Multicast control message size

Formula I came up with to calculate multicast control message size:
```
size = normal control message size
       - num bits used for single destination
       + num bits used for bitvector of destinations
```

In src/mem/ruby/network/Network.py:
```
    control_msg_size = Param.Int(8, "")
```

And in https://www.gem5.org/documentation/general_docs/ruby/garnet-2/:

> control_msg_size: The size of control messages in bytes.
> Default is 8.
> m_data_msg_size in Network.cc is set to the block size in bytes + control_msg_size.

Why is it 8?
My searches aren't turning up any reasons.

I'm guessing 8 is just an arbitrary value,
intended to be changed according to the hardware being simulated.
If this is the case, I'll need to look into real flit encoding strategies
to get a sense of what goes into a control message.

To encode a single destination on a network with 16 nodes, you need 4 bits.
To encode multiple destinations on a similar network, you need 16 bits.
In this example, 8 + 2 = 10 bytes is a reasonable control message size.

Possible revised multicast message size:
```
size = control message size
       + num bits used for bitvector of destinations
```


## 2023-04-11

### Researching multicast routing algorithms

"Interconnection networks an engineering approach",
Duato 2003

https://ufl-flvc.primo.exlibrisgroup.com/permalink/01FALSC_UFL/175ga98/alma990206564170306597

#### "5.4 Models for Multicast Communication",

- Optimal Multicast Path (OMP) -- Find the shortest path with no branches
visiting all nodes.

- Optimal Multicast Cycle (OMC) -- Similar to OMP, but the source node is also
the final destination. Satisfies the need for destination nodes to
acknowledge receipt.

- Minimal Steiner Tree (MST) -- A tree with minimal total length that connects
the source to all destination nodes.

- Optimal Multicast Tree (OMT) -- Goal is to minimize time to get from source
to destinations. Total length may be longer than MST.

The problems of finding an OMP, OMC, or MST for a 2D-mesh are NP-complete.

#### "5.5 Hardware Implementations of Multicast"

##### "5.5.2 Tree-based Multicast Routing"

This section has a really good explanation of multicast routing and hardware
requirements.

> The spanning binomial tree is suitable for networks supporting SAF or VCT
> switching techniques. When combined with wormhole switching, tree based
> multicast routing suffers from several drawbacks. Since there is no message
> buffering at routers, if one branch of the tree is blocked, then all are
> blocked.

> Tree based multicast routing may cause a message to hold many channels for
> extended periods, thereby increasing network contention. Moreover, deadlock
> can occur using such a routing scheme.

A naive extension of the XY routing algorithm to multicast can cause deadlock:

> In a similar manner, you may attempt to extend deadlock free unicast routing
> on a 2 D mesh to encompass multicast. An extension of the XY routing method
> to include multicast is shown in Figure 5.14, in which the message is
> delivered to each destination in the manner described. As in the hypercube
> example, the progress of the tree requires that all branches be unblocked.
> For example, suppose that the header flit in Figure 5.14 is blocked due to
> the busy channel [(4, 2), (4, 3)]. Node (4, 2) cannot buffer the entire
> message. As a result of this constraint, the progress of messages in the
> entire routing tree must be stopped. In turn, other messages requiring
> segments of this tree are also blocked. Network congestion may be increased,
> thereby degrading the performance of the network. Moreover, this routing
> algorithm can lead to deadlock.

> Double Channel XY Multicast Wormhole Routing

> The following double channel XY multicast routing algorithm for wormhole
> switching was proposed by Lin, McKinley, and Ni [206] for 2 D mesh. The
> algorithm uses an extension of the XY routing algorithm, which was shown
> above to be susceptible to deadlock. In order to avoid cyclic channel
> dependencies, each channel in the 2 D mesh is doubled, and the network is
> partitioned into four subnetworks

> While this multicast tree approach avoids deadlock, a major disadvantage is
> the need for double channels. It may be possible to implement double channels
> with virtual channels; however, the signaling for multicast communication is
> more complex. Moreover, the number of subnetworks grows exponentially with
> the number of dimensions of the mesh, increasing the number of channels
> between every pair of nodes accordingly.

This is interesting:

> Tree based multicast routing is more suitable for SAF or VCT switching than
> for wormhole switching. The reason is that when a branch of the tree is
> blocked, the remaining branches cannot advance if wormhole switching is used.
> There is a special case in which tree based multicast routing is suitable for
> networks using wormhole switching: the implementation of invalidation or
> update commands in DSMs with coherent caches [224]. Taking into account the
> growing interest in these machines, it is worth studying this special case.

> Message data only require a few flits. Typically, a single 32 bit flit
> containing a memory address is enough for invalidation commands. The command
> itself can be encoded in the first flit together with the destination node
> address. An update command usually requires one or two additional flits to
> carry the value of the word to be updated. Hence, it is possible to design
> compact hardware routers with buffers deep enough to store a whole message.
> However, when multicast routing is considered, the message header must encode
> the destination addresses. As a consequence, message size could be several
> times the data size. Moreover, messages have a very different size depending
> on the number of destinations, therefore preventing the use of fixed size
> hardware buffers to store a whole message. A possible solution consists of
> encoding destination addresses as a bit string (see Section 5.5.1) while
> limiting the number of destination nodes that can be reached by each message
> to a small value. This approach will be studied in Section 5.6 and applied to
> the implementation of barrier synchronization and reduction.

An alternative "pruning method" of preventing deadlock is described, where a
some destinations of a unicast method can be split off into multiple-unicast
messages if needed. I'll need to reread this section later.

At the start of the next section:

> To support deadlock free multicast or broadcast wormhole routing, the tree
> based communication pattern does not perform well unless messages are very
> short because the entire tree is blocked if any of its branches are blocked.

This is interesting, because Paper 4 ("Multicast On-Chip Trafﬁc Analysis
Targeting Manycore NoC Design") says:

> In the MESI coherence protocol, multicast messages are mostly invalidations
> which are generated upon a write to shared data and sent to the cores that
> are currently sharing it. Invalidations are short control messages, assumed
> to be of 8 bytes in our scenario.

> short messages account for more than 99% of the multicast in average. This
> ﬁgure is rather independent of the system size and, in fact, rarely drops
> below 98%.


## 2023-04-12

### "5.5.3 Path Based Multicast Communication"

This seems to indicate path-based routing requires a list of destinations
instead of a bitvector:

> In path based routing, the header of each copy of a message consists of
> multiple destinations. The source node arranges these destinations as an
> ordered list, depending on their intended order of traversal. As soon as the
> message is injected into the network, it is routed based on the address in
> the leading header flit corresponding to the first destination. Once the
> message header reaches the router of the first destination node, the flit
> containing this address is removed by the router. Now the message is routed
> to the node whose address is contained in the next header flit.

#### "Base Routing Conformed Path"

> Deadlock avoidance is considerably simplified if unicast and multicast
> routing use the same routing algorithm. Moreover, using the same routing
> hardware for unicast and multicast routing allows the design of compact and
> fast routers. The Hamiltonian path based routing algorithms proposed in
> previous sections improve performance over multiple unicast routing. However,
> their development has been in a different track compared to e cube and
> adaptive routing. Moreover, it makes no sense sacrificing the performance of
> unicast messages to improve the performance of multicast messages, which
> usually represent a much smaller percentage of network traffic. Thus, as
> indicated in [269], it is unlikely that a system in the near future will be
> able to take advantage of Hamiltonian path based routing.

> The base routing conformed path (BRCP) model [269] defines multicast routing
> algorithms that are compatible with existing unicast routing algorithms. This
> model also uses path based routing. The basic idea consists of allowing a
> multidestination message to be transmitted through any path in the network as
> long as it is a valid path conforming to the base routing scheme. For
> example, on a 2 D mesh with XY routing, a valid path can be any row, column,
> or row column.

### Difficulties

While I was reading the ebook, a message popped up saying "this session is no
longer valid". When I reloaded the page, it said someone is reading the book
and I will have to wait. Brilliant.


## 2023-04-13

### Summary of 3 routing algorithm options

#### Double-Channel XY Multicast Wormhole Routing

An extension of XY to multicast. Requires double channels to prevent deadlock.

> It may be possible to implement double channels with virtual channels;
> however, the signaling for multicast communication is more complex.

![](./images/xy-multicast.png)

![](./images/xy-multicast-routing-pattern.png)

#### Dual-Path Routing

Nodes in the network are given an ordering. Destination addresses for a
multicast message are sorted according to this ordering. The message travels
along the path to the next destination. When that destination is reached, it is
removed from the message's list and the message is passed to the next
destination.

"Dual-path" refers to how the source initially sends two messages in opposite
directions. The book describes an extension to this using 4 directions, but it
has some disadvantages.

Note: this will not work with the bitvector destination encoding strategy. It
requires an ordered list. If implemented in gem5, it would require changing the
message size as it travels along. I don't know how hard this would be. If
necessary, we could leave the message size at its longest the entire time, but
we wouldn't see the same performance improvement.

![](./images/dual-path.png)

#### Base-Routing Conformed Path

A multicast message can be sent only if it's total path is a valid path
according to the unicast algorithm being used. Multicast messages may need to
be split into smaller multicast messages to conform to this requirment.

Note:

> if the base routing algorithm is deadlock free, the multicast routing
> algorithm is also deadlock free.

Grouping destinations into valid multicasts requires some work:

> Once the set of valid paths for multidestination messages has been
> determined, it is necessary to define routing algorithms for collective
> communication. The hierarchical leader based (HL) scheme has been proposed in
> [269] to implement multicast and broadcast. Given a multicast destination
> set, this scheme tries to group the destinations in a hierarchical manner so
> that the minimum number of messages is needed to cover all the destinations.

The grouping algorithm is descibed next in the book.

![](./images/brcp.png)

### Easiest option to implement

All 3 options probably could be successfully implemented. If I had to guess,
double-channel XY would take the least amount of work to do so.

### More options

The 2003 Duato book listed a few more routing algorithms, but they were either
more complex or specialized.

2003 was a long time ago; better algorithm have probably been found since then.
The book provides citations for the algorithms it discusses.
I can look for them on IEEE and look for newer papers that cited them proposing better algorithms.



## 2023-05-01

I'm looking at flitisizeMessage in NetworkInterface.cc.
One RouteInfo struct is created for the message, and passed with each flit.
RouteInfo is defined in CommonTypes.hh, and has the following attributes:
```
    // destination format for table-based routing
    int vnet;
    NetDest net_dest;

    // src and dest format for topology-specific routing
    int src_ni;
    int src_router;
    int dest_ni;
    int dest_router;
    int hops_traversed;
```
net_dest, dest_ni, and dest_router are problematic.
In my initial survey of the code, I didn't notice this.
RouteInfo was designed with only one destination in mind,
and will need to be modified to allow for multiple destinations.

I'll need to change each of these single variables to vectors,
and I'll also need to change every part of the code that references these to treat
them as vectors (even if they are just vectors of length 1 in multiple-unicast mode).
Here's where RouteInfo is mentioned:
```
$ grep -lr RouteInfo
simple/PerfectSwitch.cc
simple/routing/WeightBased.cc
simple/routing/BaseRoutingUnit.hh
simple/routing/WeightBased.hh
garnet/Router.cc
garnet/GarnetNetwork.cc
garnet/NetworkInterface.cc
garnet/Router.hh
garnet/RoutingUnit.cc
garnet/flit.hh
garnet/GarnetNetwork.hh
garnet/RoutingUnit.hh
garnet/CommonTypes.hh
garnet/flit.cc
garnet/Credit.cc
```

Hopefully the required changes aren't too involved.
I created a new branch, RouteInfo-modification, so I don't mess up my working code.



## 2023-05-02

These files all mention the problematic elements of RouteInfo:
```
$ grep -lr 'net_dest\|dest_ni\|dest_router'
garnet/GarnetNetwork.cc
garnet/NetworkInterface.cc
garnet/RoutingUnit.cc
garnet/RoutingUnit.hh
garnet/CommonTypes.hh
garnet/flit.cc
```
I think its safe to ignore the other files in my list from yesterday.

Here are the lines as well:
```
$ grep -r 'net_dest\|dest_ni\|dest_router'
GarnetNetwork.cc:    int dest_node = route.dest_router;
NetworkInterface.cc:        route.net_dest = new_net_msg_ptr->getDestination();
NetworkInterface.cc:        route.dest_ni = destID;
NetworkInterface.cc:        route.dest_router = m_net_ptr->get_router_id(destID, vnet);
RoutingUnit.cc:    if (route.dest_router == m_router->get_id()) {
RoutingUnit.cc:        outport = lookupRoutingTable(route.vnet, route.net_dest);
RoutingUnit.cc:            lookupRoutingTable(route.vnet, route.net_dest); break;
RoutingUnit.cc:            lookupRoutingTable(route.vnet, route.net_dest); break;
RoutingUnit.cc:    int dest_id = route.dest_router;
RoutingUnit.hh:    int  lookupRoutingTable(int vnet, NetDest net_dest);
CommonTypes.hh:        : vnet(0), src_ni(0), src_router(0), dest_ni(0), dest_router(0),
CommonTypes.hh:    NetDest net_dest;
CommonTypes.hh:    int dest_ni;
CommonTypes.hh:    int dest_router;
flit.cc:    out << "Dest NI=" << m_route.dest_ni << " ";
flit.cc:    out << "Dest Router=" << m_route.dest_router << " ";
```


### GarnetNetwork.cc

So there's this function called "update_traffic_distribution".
I'm guessing its used for recording statistics.
```
void
GarnetNetwork::update_traffic_distribution(RouteInfo route)
{
    int src_node = route.src_router;
    int dest_node = route.dest_router;
    int vnet = route.vnet;

    if (m_vnet_type[vnet] == DATA_VNET_)
        (*m_data_traffic_distribution[src_node][dest_node])++;
    else
        (*m_ctrl_traffic_distribution[src_node][dest_node])++;
}
```

I am correct. Looking at GarnetNetwork.hh, I find:
```
    std::vector<std::vector<statistics::Scalar *>> m_data_traffic_distribution;
    std::vector<std::vector<statistics::Scalar *>> m_ctrl_traffic_distribution;
```

To make this work for multiple destinations,
I think I'll have it loop through the destinations and update the corresponding
part of the traffic distribution.
But first, I should look to see where update_traffic_distribution is called to
see if this plan makes sense.

update_traffic_distribution is called in one place: flitisizeMessage.

I think my plan is reasonable.

### NetworkInterface.cc

This is where flitisizeMessage is, which was the original cause for these changes.

### RoutingUnit.cc

This one is tricky.
This file is where the 'outportCompute' functions are.

TABLE and XY can only be used with multiple-unicast,
so whatever changes I make need to leave these alone.

The first issue is the fact that outportCompute only returns one outport.
To handle multicast, outportCompute needs to be able to return multiple outports,
each with a subset of the destinations associated with them.
As in, these destinations need to be sent through this outport,
and those destinations through that outport.

outportCompute is called by one other function, route_compute (in Router.cc).
route_compute is called by 'wakeup' in InputUnit.cc.
Here's the problem area:
```
            // Route computation for this vc
            int outport = m_router->route_compute(t_flit->get_route(),
                m_id, m_direction);

            // Update output port in VC
            // All flits in this packet will use this output port
            // The output port field in the flit is updated after it wins SA
            grant_outport(vc, outport);

```
In this code, the outport is computed from the head flit,
and every flit is sent along that outport.
What needs to happen, is that the set of outports with corresponding destinations
is computed, and every flit is sent to every outport with only the
corresponding destinations.

Essentially, if the message needs to split off at this point, new RouteInfos
need to be constructed and associated with the flits.

The above code calls grant_outport, which is defined in InputUnit.hh:
```
    inline void
    grant_outport(int vc, int outport)
    {
        virtualChannels[vc].set_outport(outport);
    }
```
This is also problematic, because each virtual channel can only have one outport
associated with it here.
```
    // Input Virtual channels
    std::vector<VirtualChannel> virtualChannels;
```



## 2023-05-05

### RoutingUnit.hh

Just a function prototype.

### CommonTypes.hh

Contains the definition of RouteInfo.

### flit.cc

Print statement for debugging. Easy to change.

### Summary

It seems like the main issues are:

- Modify all of the destination variables to allow for storing multiple
destinations.

- The routers are currently designed to pass flits forward, unchanged. However,
with multicast the routers need to remove destinations when they are reached
and split destinations sets at branches. The flit object provides a set_route
method, which we can use for this. It is unknown to me if there is any code is
based on the assumption the destination of a flit does not change.

- The code currently allows granting only one outport to a virtual channel at a
time. In order to allow branching with wormhole routing, a virtual channgel
needs up to two outports at once. This seems like the type of change that could
allow deadlock to occur.

From https://www.gem5.org/documentation/general_docs/ruby/garnet-2/

> vcs_per_vnet: number of virtual channels (VC) per virtual network. Default is
> 4. This can also be set from the command line with –vcs-per-vnet.

> Flow Control
>
> Virtual Channel Flow Control is used in the design. Each VC can hold one
> packet. There are two kinds of VCs in the design - control and data. The
> buffer depth in each can be independently controlled from GarnetNetwork.py.
> The default values are 1-flit deep control VCs, and 4-flit deep data VCs.
> Default size of control packets is 1-flit, and data packets is 5-flit.

From the Interconnection Networks book:

> The algorithm uses an extension of the XY routing algorithm, which was shown
> above to be susceptible to deadlock. In order to avoid cyclic channel
> dependencies, each channel in the 2 D mesh is doubled, and the network is
> partitioned into four subnetworks, N_(+X,+Y), N_(+X,Y), N_(X,+Y), and N_(X,Y).

> While this multicast tree approach avoids deadlock, a major disadvantage is
> the need for double channels. It may be possible to implement double channels
> with virtual channels; however, the signaling for multicast communication is
> more complex.

Garnet already has multiple virtual channels, but separates their use according
to packet type. The multicast-XY algorithm requires them to be separated
according to direction of packet travel. So the number of virtual channels in
garnet will still need to be doubled.

I am not completely sure why gem5 uses separate VCs for control and data
packets, but my guess is that it prevents control packets from being slowed
down during large data transfers in the network. I do not know if this is an
just an optimization, or if it is necessary to prevent deadlock.
