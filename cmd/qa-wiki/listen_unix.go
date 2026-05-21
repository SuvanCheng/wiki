//go:build !windows

package main

import (
	"context"
	"net"
	"syscall"
)

func listen(addr string) (net.Listener, error) {
	lc := net.ListenConfig{
		Control: func(network, address string, c syscall.RawConn) error {
			var operr error
			c.Control(func(fd uintptr) {
				operr = syscall.SetsockoptInt(int(fd), syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1)
			})
			return operr
		},
	}
	return lc.Listen(context.Background(), "tcp", addr)
}
