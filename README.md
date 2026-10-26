# Distributed Key-Value Store

A key-value store is a simple kind of database: you give it a key (like "Rowan Schlotterbeck") and a value (like "Student"), and it remembers that pairing so you can look it up later. This project is a key-value store that runs on several computers at once instead of just one. If one of those computers crashes, the others keep working and no data is lost. It does this using a well-known method for keeping multiple machines in agreement, called the Raft Consensus Algorithm.

It is written from scratch in C++. Below is a a web page where you can watch the machines elect a leader and recover from a crash in real time.

https://raft-visual.netlify.app 



## What it does

- Several machines (called nodes) work together as one store.
- The nodes automatically pick one of themselves to be in charge (the leader). If the leader stops working, the others notice and pick a new one on their own, usually in about a quarter of a second.
- When you save something, it is not confirmed until a majority of the machines have safely recorded it. This is what makes sure your data is not lost if one machine dies right after.
- Every machine keeps its own copy of all the data, so the group stays in sync.
- If a machine crashes and restarts, it automatically catches back up with the others.
- Data is written safely to disk, so even pulling the power plug does not lose anything that was confirmed.

## Try it in 60 seconds

You will need CMake, a C++20 compiler, and the Asio library.

Build it:

```
cmake -S . -B build
cmake --build build -j
```

Start three machines, each in its own terminal window:

```
./build/kv_server data/n1.log --cluster config/cluster.conf --node-id 1 --client-port 5551 --status-port 8081
./build/kv_server data/n2.log --cluster config/cluster.conf --node-id 2 --client-port 5552 --status-port 8082
./build/kv_server data/n3.log --cluster config/cluster.conf --node-id 3 --client-port 5553 --status-port 8083
```

Within about a second, one of them becomes the leader. Now the interesting part: press Ctrl-C on the leader to "kill" it. Watch the other two. In a fraction of a second, one of them takes over as the new leader, and the store keeps working. That recovery, happening on its own with no human involved, is the whole point of the project.

If you would rather not install anything, the **[visualizer link]** lets you watch all of this in your browser.

## How it is put together

The store is a group of identical machines. At any moment one of them is the leader and the rest are followers. All saves go through the leader, which then passes them along to the followers. Reads are answered by the leader too.

Each machine keeps three things:

- A log, which is an ordered list of every change ever made. Think of it as a journal that is only ever added to.
- The actual data, which is just the result of replaying that journal from the top.
- A record of who it has voted for and what round of elections it is on, so it behaves correctly even after a restart.

Each machine listens on three separate channels: one for talking to clients (the people saving and reading data), one for talking to the other machines, and one read-only channel that simply reports the machine's current status, which is what the visualizer reads.

Everything on a single machine runs on one thread. This was a deliberate choice. It means the machine does one thing at a time in a predictable order, which removes a whole category of bugs that come from several things happening at once.

## How the agreement works

**Picking a leader.** Each follower waits a short, random amount of time to hear from a leader. If it hears nothing, it assumes there is no leader and puts itself forward, asking the others to vote for it. Whoever gets votes from more than half the group becomes the leader. The randomness matters: it keeps the machines from all volunteering at the same instant and splitting the vote.

**Staying leader.** The leader sends a quick "still here" message to everyone several times a second. As long as the followers keep hearing it, they stay followers. If those messages stop, a new election begins.

**Saving data.** When you save something, the leader writes it to its own journal, then sends it to the followers. Once more than half the machines have safely recorded it, the leader marks it as official, updates its own data, tells the others to do the same, and only then tells you it worked. Because a majority already has it, the save survives even if the leader dies the very next moment.

**Catching up after a crash.** A machine that falls behind, whether it crashed or just missed some messages, is brought back up to date by the leader automatically. The leader figures out where the two of them last agreed and sends over everything the follower is missing.

**Surviving restarts.** Before a machine confirms anything important, it writes that fact to disk first. So if it crashes and comes back, it remembers what it had already promised and does not contradict itself.

## Decisions worth explaining

**How data is packaged for saving and sending.** I used a widely used format called Protocol Buffers rather than inventing my own. The main reason is that it would let someone write one of these machines in a different programming language later, and it would still understand the others. It is also a standard tool that real distributed systems use.

**How saved data is checked for damage.** Each entry in the journal carries a small checksum, which is a number that lets a machine notice if the data got corrupted on disk. I used a lightweight kind called CRC32C rather than a heavy cryptographic one, such as SHA-256. The heavy kind is designed to stop a malicious person from secretly altering data, which is not the problem here. The problem here is ordinary disk corruption, and the lightweight check catches that perfectly while being much faster.

**One thing at a time.** As mentioned above, each machine runs on a single thread. This trades away some raw speed in exchange for being far easier to reason about and get correct.

**Safety over speed when saving.** Every save waits for the data to be truly written to the physical disk before confirming. This makes each save slower, but it is the only way to honestly promise that confirmed data survives a power loss.

## How it was tested

Correctness was not assumed, it was thoroughly checked. The tests include:

- Deliberately crashing machines at the worst possible moments and confirming they recover correctly.
- Cutting the journal file off at every possible point to confirm a half-written entry is handled safely rather than corrupting everything.
- Reproducing two tricky scenarios (Figure 7 & 8) straight from the original Raft research paper to confirm the system behaves the way the paper says it should.
- Running the code under tools that detect memory mistakes, with no problems found.

The fault-tolerance results below come from a separate benchmark that drives a real group of machines, kills and freezes them at random while saving data, and then checks that nothing was lost.

## Benchmarks

All numbers are from a MacBook Pro (Apple Silicon), with the default settings: an election timeout between 150 and 300 milliseconds and a heartbeat every 50 milliseconds.

**How fast it recovers from a leader failure.** The leader was killed while the store was handling a steady stream of saves, and this was repeated 50 times on a 3-machine group. The group picked a new leader and resumed saving in a median of about 250 milliseconds, with 90 percent of recoveries finishing under about 305 milliseconds. A few took longer (up to about 570 milliseconds). Those were the cases where the first vote was split and a second round was needed, which is normal. Recovery time is set mainly by how long a machine waits before assuming the leader is gone, which here is 150 to 300 milliseconds, so these numbers line up with what the design predicts. A 5-machine group behaved similarly, with a median around 280 milliseconds and a tighter worst case.

**Whether it loses data under stress.** The store was put through 60 seconds of deliberate chaos: machines were killed and restarted, and frozen and thawed, at random, while saves ran the whole time. Afterwards everything was brought back and the group was allowed to settle, then every save that had been confirmed to the client was checked.

- 3 machines: 1,284 confirmed saves, 0 lost, no disagreement between machines, across 21 injected failures.
- 5 machines: 872 confirmed saves, 0 lost, no disagreement between machines, across 38 injected failures.

Saves that were never confirmed (because a machine died mid-request) are reported separately and not counted either way, since the client genuinely cannot know whether those landed.

The checker itself was tested by planting known problems: a save that was never really made, a secretly deleted key, and a single corrupted byte in one machine's records. It caught all of them.

## What it does not do yet (Future Work)

Being clear about the edges:

- The journal grows forever. There is no cleanup of old entries yet (a feature usually called snapshotting).
- The group of machines is fixed when it starts. You cannot add or remove machines while it is running.
- A machine that has fallen a long way behind catches up slowly, because it re-checks agreement one step at a time. In testing, a machine about 900 entries behind took around 30 to 50 seconds to catch up. There is a well-known fix for this (sending a hint about where to resume) that is not implemented yet.
- Reads are always answered by the leader and are not optimized for the strictest possible consistency guarantees.
- It is set up to run on one computer for demonstration. Running it across real separate machines would need configuration changes.

## References

- The Raft paper: "In Search of an Understandable Consensus Algorithm" by Diego Ongaro and John Ousterhout. https://raft.github.io/raft.pdf
- More about Raft: https://raft.github.io/
- The companion visualizer, which lets you watch all of this happen in a browser: https://raft-visual.netlify.app 