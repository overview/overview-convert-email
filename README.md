This is an [Overview](https://github.com/overview/overview-server) converter.
It extracts a single email (`message/rfc822`) into its body and attachments.

Usage
=====

In an Overview cluster, you'll want to use the Docker container:

`docker run -e POLL_URL=http://worker-url:9032/Email overview/overview-convert-email:2.1.0`

Developing
==========

`docker build .` will compile and run unit tests. The tests are in
`test/test-*`.

`./dev` will connect to the `overviewserver_default` network and run with
`POLL_URL=http://overview-worker:9032/Email`.

Design decisions
----------------

We parse using [GMime](https://github.com/jstedfast/gmime): it's fast, widely
used, well-designed and well-documented. (In particular, it handles charset
conversion and reformats addresses and dates.) Best of all, it's a streaming
library: we couldn't bring its usage above 10MB, even when testing large
files. The downside: we use C.

Output is deterministic and it's a single file. That makes testing easy.

License
-------

This software is Copyright 2011-2018 Jonathan Stray, and distributed under the
terms of the GNU Affero General Public License. See the LICENSE file for details.
