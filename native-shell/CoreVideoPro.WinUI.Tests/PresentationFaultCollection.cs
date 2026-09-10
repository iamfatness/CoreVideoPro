using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// Fault-injection seams are PROCESS-WIDE by nature (one shared D3D device, one armed
/// flag). xUnit runs test classes in parallel by default, which had one class disarming
/// another's seam mid-case. Every class that arms a fault joins this collection so they
/// run one at a time.
/// </summary>
[CollectionDefinition(Name, DisableParallelization = true)]
public sealed class PresentationFaultCollection
{
    public const string Name = "PresentationFaultInjection";
}
