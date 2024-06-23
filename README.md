# compile
```
gcc select.c -o select
gcc epoll.c -o epoll
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
