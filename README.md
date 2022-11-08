
sertee
======

sertee provides multiple "copies" of a character device using the CUSE
(character device in userspace) interface of the Linux kernel.

If, for example, you have a character device connected to a serial/UART port and
multiple readers, you can connect all readers to this device but every reader
will only get a part of the device's output. Sertee can create a character
device for each reader so each one receives a complete copy of the output of
the original device.

```
usage: sertee [options]

options:
    --help|-h             print this help message
    --name=NAME|-n NAME   device names (mandatory)
    --source=NAME|-S NAME source device name (mandatory)
    --bufsize=SIZE        size of internal buffer (default: 1024 bytes)
```

Example
-------

If there is a device `/dev/ttyUSB0` and two processes want to access its
output, calling

`./sertee --name=uart0,uart1 -S /dev/ttyUSB0 -s`

will create two additional devices `/dev/uart0` and `/dev/uart1`. The parameter
`-s` avoids a warning as sertee is single-threaded only for now.
