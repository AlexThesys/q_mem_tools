# Quick Memory Tools  
**Version 0.3.8**  

## ==== Command Line Options ====  

***The program has to be launched in either process or dump inspection mode.***  

`-p` || `--process`	-- launch in process inspection mode  
`-d` || `--dump`	-- launch in dump inspection mode  

`-t=<num_threads>` || `--threads=<num_threads>`	-- limit the number of worker threads  
`-b=<N>` || `--block_info=<N>`	-- alloc block_info size == (dwAllocationGranularity * N), N=[1-8]  
`-f` || `--show-failed-readings`	-- show the regions that failed to be read (process mode only)  
`-h` || `--help`	-- show help (this message)  
`-v` || `--version`	-- show version<br/>
`-n` || `--no-page-caching`	-- force disable page caching (dump mode only)<br/>
`-c` || `--clear-standby-list`	-- clear standby physical pages (dump mode only)<br/>
`-s || --disable-symbols` -- disable symbol resolution<br/>

## ==== Common Commands ====  

`?`	- list commands (this message)<br/>
`clear`	- clear screen  
`q` || `exit`	- quit program<br/> 
`/ <pattern>`	- search for a hex string  
`/x <pattern>`	- search for a hex value (1-8 bytes wide)  
`/a <pattern>`	- search for an ASCII string 
  *  All search commands have optional `:i`|`:s`|`:o` modifiers to limit the search to image || stack || other<br/>
  ** Alternatively search could be ranged (e.g. `/x@<start-address>:<length> <pattern>` )

`xb@<address>:<N>`	- hexdump N bytes at address  
`xw@<address>:<N>`	- hexdump N words at address  
`xd@<address>:<N>`	- hexdump N dwords at address  
`xq@<address>:<N>`	- hexdump N qwords at address<br/>
`x(b|w|d|q)@<address>:<N>^<hex-string>` - XOR hex-data with a hex-string<br/>
`x(b|w|d|q)@<address>:<N>&<hex-string>` - AND hex-data with a hex-string<br/>
`> <file-path>` - redirect output to a file, overwrite data<br/>
`>a <file-path>` - redirect output to a file, append data<br/>
`> stdout` - redirect output to stdout<br/>
`%entropy@<address>:<size>` - calculate entropy of a block<br/>
`%crc32c@<address>:<size>`  - calculate the crc32c of a block<br/>
`im@<address>` - inspect memory region<br/>
`iM <name>` - inspect module<br/>
`it <tid>` - inspect thread<br/>
`ii <file-path>` - inspect image<br/>
`lM`	- list process modules  
`lt`	- list process threads  
`lmi`	- list memory regions info  
`lmic`	- list committed memory regions info<br/>
`s@<address>` - resolve symbol at address<br/>
`sn <name>` - resolve symbol by <name><br/>
`sf` 	- show next symbol<br/>
`sb` 	- show previous symbol<br/>
`sp<path0;path1;..>` - set symbol search paths (separated by ';')<br/>
`sp`    - get symbol search paths<br/>  
  *  Memory listing commands have optional `:i`|`:s`|`:o` modifiers to display only image || stack || other<br/>

## ==== Process Mode Commands ====  

`p <pid>`	- select PID  
`lp`	- list system PIDs  
`imu`	- show memory usage  
`th`	- traverse process heaps (slow)  
`the`	- traverse process heaps, calculate entropy (slower)  
`thb`	- traverse process heaps, list heap blocks (extra slow)  

## ==== Crash Dump Mode Commands ====  

`/xr <pattern>`	- search for a hex value in GP registers  
`ltr`	- list thread GP registers  
`lm`	- list memory regions (regions to search through)<br/>
`lh` - list handles
