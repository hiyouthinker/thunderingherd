# compile
```
gcc select.c -o select
gcc epoll.c -o epoll
gcc accept.c -o accept
```
# test
## select
```
# use flock
sudo ./select -w 2 -p 9000 -f

# use nonblock
sudo ./select -w 2 -p 9000 -n
```
## epoll
```
# use EPOLLEXCLUSIVE flag
sudo ./epoll -w 2 -p 9000
```
## accept
```
# for kernel version >= 2.6, accept does not have the thundering herd problem
sudo ./accept -w 2 -p 9000
```
