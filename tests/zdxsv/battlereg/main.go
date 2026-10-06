// battlereg registers one battle with a zdxsv battle server, as the lobby does
// before it sends BattleStart (pkg/lobby startBattle -> Logic.NotifyBattleUsers,
// net/rpc over HTTP, gob). Types mirror pkg/battle/battlerpc (gob matches by name).
//
//	battlereg 127.0.0.1:3080 SESSION:USERID:NAME:ENTRY ...
package main

import (
	"fmt"
	"net/rpc"
	"os"
	"strconv"
	"strings"
)

type User struct {
	UserID    string
	SessionID string
	Name      string
	Team      string
	Entry     byte
	P2PMap    map[string]struct{}
}

type BattleInfoArgs struct {
	Users []User
}

func main() {
	if len(os.Args) < 3 {
		fmt.Fprintln(os.Stderr, "usage: battlereg RPCADDR SESSION:USERID:NAME:ENTRY ...")
		os.Exit(2)
	}
	args := BattleInfoArgs{}
	for _, a := range os.Args[2:] {
		f := strings.SplitN(a, ":", 4)
		if len(f) != 4 {
			fmt.Fprintln(os.Stderr, "bad user", a)
			os.Exit(2)
		}
		e, _ := strconv.Atoi(f[3])
		args.Users = append(args.Users, User{SessionID: f[0], UserID: f[1], Name: f[2], Entry: byte(e), P2PMap: map[string]struct{}{}})
	}
	c, err := rpc.DialHTTP("tcp", os.Args[1])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	var reply int
	if err := c.Call("Logic.NotifyBattleUsers", args, &reply); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	fmt.Println("registered", len(args.Users), "users")
}
