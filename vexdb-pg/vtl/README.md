# VexDB template library
TBA

# Memory Containers
### note
 - You are free to modify the code, but do not change the existing behavior.
 - The containers has more or less functionalities comparing to stl, and there are a few behaviors
   that are different, such as Vector::Vector(size_t).
 - All files ending with .hpp is still under development, no guarentee on consistancies.

## Usage
Same with stl, include them using `#include <vtl/xxx>`.
The recommanded include order is listed below:
1. STL/BOOST and other external libraries
2. VTL memory containers
3. VTL disk containers
4. OG header files
An exmaple can be
```c++
#include <algorithm>
#include <vtl/vector>
#include <vtl/disk_container/diskvector.hpp>
#include "postgres.h"
```

All containers need to be manually released as PG which uses setjmp is incompatible with RAII,
call `.destroy()` to release them. Read <vtl/allocator> for memory allocation setting, which is
basically a wrapper of `MemoryContext`.
