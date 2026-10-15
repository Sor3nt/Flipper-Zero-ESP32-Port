# Receive stability

See [the current port notes](T-EMBED-PORT.md) for the 3.6.3 implementation and
validation limits. Incoming records preserve the UI's borrowed capture reader;
live RX snapshots stay in bounded RAM and UI operations persist them as needed.

Run `python3 tests/protopirate_rx/run.py` for the production history regression.
The harness uses real concurrent host threads and storage/decoder doubles.
