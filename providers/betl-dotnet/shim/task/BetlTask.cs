/* Base class for user-defined dotnet.task scripts.
 *
 * User script convention: declare a public class named `UserTask`
 * that extends BetlTask and overrides Run(). The entry point in
 * Entry.cs instantiates and runs it.
 *
 * Example:
 *
 *   public class UserTask : BetlTask {
 *       public override void Run() {
 *           Log.Info("hello from C#");
 *       }
 *   }
 *
 * VB.NET equivalent (v0.2 phase 2):
 *
 *   Public Class UserTask
 *       Inherits BetlTask
 *       Public Overrides Sub Run()
 *           Log.Info("hello from VB.NET")
 *       End Sub
 *   End Class
 */

namespace Betl;

public abstract class BetlTask
{
    public abstract void Run();
}
