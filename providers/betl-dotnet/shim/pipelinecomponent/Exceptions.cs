/* Exceptions surfaced from shim code into user PipelineComponent
 * code. Mirrors the shape of Microsoft.SqlServer.Dts.Pipeline's
 * own exception hierarchy enough for catch-blocks in ported code
 * to keep working. */

using System;

namespace Microsoft.SqlServer.Dts.Pipeline;

public class BetlPipelineException : Exception
{
    public BetlPipelineException(string message) : base(message) { }
    public BetlPipelineException(string message, Exception inner) : base(message, inner) { }
}
