
# Adding Multicast Support to Gem5

## Plan

- Start from a clean gem5 download.

- Follow gem5 code style guide and commit message format.

- Do not delete existing code.
  The current functionality should be maintained.
  Enabling the new multicast functionality on a simulation should be optional.

- Decide on a flit encoding strategy (destination list, bitvector, or other).
  Even though gem5 does not actually encode the message data into flits,
  this information is needed to simulate the correct number of flits.
  Figure out if the new encoding strategy can be applied to unicast messages as well.

- Decide on a routing algorithm to implement.
  Pick the simplest one; better algorithms can be implemented later.
  Figure out if the new algorithm can be applied to unicast messages as well.

- Add a command-line argument for choosing between multiple-unicast and multicast.

- In file `src/mem/ruby/network/garnet/NetworkInterface.cc`, func `flitisizeMessage`:
  Add code for producing a single sequence of flits for a multicast message.

- In file `src/mem/ruby/network/Network.cc`, func `MessageSizeType_to_int`:
  Modify to return the correct size for a multicast message.
  It appears there is already `MessageSizeType_Multicast_Control`,
  but the function currently returns the same value as for a standard control packet.

- In `src/mem/ruby/network/garnet/RoutingUnit.cc`, `Router.cc`, `InputUnit.cc`,
  and the corresponding `.hh` files:
  Modify routers to check for multicast packets.
  Implement new routing algorithm.

- Add statistics output as needed.

- Prepare tests.
