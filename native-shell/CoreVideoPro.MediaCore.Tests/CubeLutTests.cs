using CoreVideoPro.MediaCore.Models;
using System.Security.Cryptography;
using System.Text;
using Xunit;
namespace CoreVideoPro.MediaCore.Tests;
public sealed class CubeLutTests
{
    private const string Identity="TITLE \"Identity\"\nLUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
    [Fact] public void DurableCubeContentAndHashValidateAndDoNotRequireAPath()
    {
        var parsed=CubeLutParser.Parse(Identity);Assert.Equal(2,parsed.Size);Assert.Equal(8,parsed.Samples.Length);
        var operation=new GradeOperation { Kind="cube",CubeText=Identity,CubeSha256=Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(Identity))).ToLowerInvariant() };
        new AdvancedGradeDocument { Operations=[operation] }.Validate();
        Assert.Throws<ArgumentException>(()=>new AdvancedGradeDocument { Operations=[operation with { CubeSha256=new string('0',64) }] }.Validate());
    }
    [Theory] [InlineData("LUT_3D_SIZE 65\n")] [InlineData("LUT_1D_SIZE 2\n")]
    [InlineData("LUT_3D_SIZE 2\nNaN 0 0\n")] [InlineData("LUT_3D_SIZE 2\nDOMAIN_MIN 1 1 1\nDOMAIN_MAX 0 0 0\n")]
    public void InvalidDimensionsDomainNonFiniteAndIncompleteSamplesFail(string content) => Assert.Throws<ArgumentException>(()=>CubeLutParser.Parse(content));
}
