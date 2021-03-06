# ecmh - Easy Cast du Multi Hub

[Easy Cast du Multi Hub (ecmh)](http://unfix.org/projects/ecmh/)
is a network daemon that relays and aggregates multicast packets from/to network interfaces.

This allows IPv6 multicast routing on Linux and other OS's that
do not implement IPv6 multicast routing. It also allows IPv4 to
IPv6 and IPv6 to IPv4 translation of multicast traffic.
Allowing multicast where it is not available at the moment.
It also has a tunnel mode so that it can detect multicast
packets being tunneled inside protocol 41 (IPv6 in IPv4)
tunnels [RFC3056](https://tools.ietf.org/html/rfc3056).

This is for instance very handy for
projects like [SixXS](http://www.sixxs.net) where we can
install this daemon on the POPs and let it easily do
multicast IPv6 allowing the m6bone (http://www.m6bone.net)
to grow and provide IPv6 multicast and because of the
translation also IPv4 multicast everywhere we would want it to be.

For a larger explaination see the [ecmh webpage](http://unfix.org/projects/ecmh/).

## Compilation

* Linux
  Either get the debian package or use ```make help``` in this directory (not src)
* FreeBSD 5 (GNU make)
  use ```make help``` to see the options
* FreeBSD 5 (BSD make)
  ```cd src;``` uncomment the 3 lines in the Makefile to get it working
* FreeBSD 4
  ```cd src;``` uncomment the 3 lines in the Makefile to get it working
  Requires the libgnugetopt port. Make it from the 'src' dir'

## License

The license for ecmh is the BSD 3-clause license.

## Author

The author of ecmh is [Jeroen Massar](http://jeroen.massar.ch).

