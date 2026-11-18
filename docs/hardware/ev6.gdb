set pagination off
set confirm off
target extended-remote :3333
monitor reset halt
hbreak *0x08000406
continue
printf "hit1 flag=0x%x status=0x%04x attempts=%u\n", *(unsigned*)($r7+4), *(unsigned*)($r7+8), *(unsigned*)($r7+12)
continue
printf "hit2 flag=0x%x status=0x%04x attempts=%u\n", *(unsigned*)($r7+4), *(unsigned*)($r7+8), *(unsigned*)($r7+12)
continue
printf "hit3 flag=0x%x status=0x%04x attempts=%u\n", *(unsigned*)($r7+4), *(unsigned*)($r7+8), *(unsigned*)($r7+12)
delete
monitor reset run
detach
