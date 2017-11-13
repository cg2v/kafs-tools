# kafs-tools
Tools and libraries for the kafs implementation of the AFS distributed filesystem (work in progress)

## aklog
This is a simple aklog program that sets tokens for kafs and other AF_RXRPC users. It accepts a cell name and realm name. No other aklog switches are accepted. There is both a des-only and rxkad-kdf version of aklog included, the latter for use in post 2013 cells which have removed the ability of their kerberos kdcs to distribute des session keys.
aklog is based on klog code from the kafs author and is licensed under the GPL

## Rxrxrpc
Rxrxrpc is a library that aims to implement the afs librx and librxkad interface on top of AF_RXRPC sockets on linux. A basic test client that communicates with a ptserver is present. RX server support is incomplete.
