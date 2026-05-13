/* The SSIS DataType enum, declared in full so user code that
 * references any value compiles, but only a subset is exercised
 * by the Phase 1a runtime (i8/r8/bool/wstr). Unsupported values
 * surface at runtime in PipelineBuffer.GetXxx / SetXxx with
 * BetlPipelineException.
 *
 * Numeric values match Microsoft's published enum so reflection
 * over DataType.* by integer ID keeps working in ported code. */

namespace Microsoft.SqlServer.Dts.Pipeline.Wrapper;

public enum DataType
{
    DT_EMPTY            = 0,
    DT_NULL             = 1,
    DT_I2               = 2,
    DT_I4               = 3,
    DT_R4               = 4,
    DT_R8               = 5,
    DT_CY               = 6,
    DT_DATE             = 7,
    DT_BOOL             = 11,
    DT_DECIMAL          = 14,
    DT_UI1              = 17,
    DT_UI2              = 18,
    DT_UI4              = 19,
    DT_I8               = 20,
    DT_UI8              = 21,
    DT_FILETIME         = 64,
    DT_GUID             = 72,
    DT_BYTES            = 128,
    DT_STR              = 129,
    DT_WSTR             = 130,
    DT_NUMERIC          = 131,
    DT_DBDATE           = 133,
    DT_DBTIME           = 134,
    DT_DBTIMESTAMP      = 135,
    DT_IMAGE            = 136,
    DT_TEXT             = 200,
    DT_NTEXT            = 201,
    DT_DBTIMESTAMP2     = 202,
    DT_DBTIMESTAMPOFFSET= 203,
    DT_DBTIME2          = 204,
    DT_I1               = 205,
}
