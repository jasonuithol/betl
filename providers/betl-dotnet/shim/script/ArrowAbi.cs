/* Arrow C Data Interface struct layouts.
 *
 * We don't need ArrowSchema at runtime (the AOT'd .so has its
 * schema baked in via codegen — input columns are accessed by
 * known ordinal). Only ArrowArray is touched at runtime: the
 * plugin hands us a pointer to the top-level struct array and we
 * walk into its children. */

using System;
using System.Runtime.InteropServices;

namespace Betl;

[StructLayout(LayoutKind.Sequential)]
public unsafe struct ArrowArray
{
    public long  Length;
    public long  NullCount;
    public long  Offset;
    public long  NBuffers;
    public long  NChildren;
    /* `const void **buffers` on the C side — array of NBuffers
     * pointers. */
    public void** Buffers;
    /* `struct ArrowArray **children` — array of NChildren pointers. */
    public ArrowArray** Children;
    public ArrowArray*  Dictionary;
    public IntPtr       Release;
    public void*        PrivateData;
}
