* Based off of Mellanox FreeBSD-OFED 2.1.6
* Compiles under 11.0-Release-p1 under AMD64, at least
* Mostly working, some counters are not being updated under sysctl hw.mlxenX.stat (only the packet <> X byte counters) -- 
other counters seem to be OK
* Rate-limiting / queue placement works properly, workaround for deprecated M_FLOWID flag
* Fixed fget_unlocked() and fdclose() calls to work with 11.x
* Some minor fixes needed (Makefile) to be less kludgy, some more #if FreeBSDVersion < X defines around blocks so it can be 
freely compiled between 10.x and 11.x

