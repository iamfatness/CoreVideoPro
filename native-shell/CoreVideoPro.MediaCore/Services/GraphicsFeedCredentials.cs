namespace CoreVideoPro.MediaCore.Services;

public sealed class GraphicsFeedCredentialException() : IOException("Authorize this graphics feed locally.");

public static class GraphicsFeedCredentials
{
    public static bool IsValidBearer(string? token)
    {
        if (token is not { Length: > 0 and <= 4096 }) return false;
        var body = token.AsSpan().TrimEnd('=');
        foreach (var ch in body)
            if (!char.IsAsciiLetterOrDigit(ch) && ch is not ('-' or '.' or '_' or '~' or '+' or '/')) return false;
        return body.Length > 0;
    }

    public static void ValidateEndpoint(GraphicsFeedDefinition feed)
    {
        if (feed.Format == "wss") GraphicsFeedTransport.ValidateWebSocketEndpoint(feed);
        else if (feed.Format == "custom-json") GraphicsFeedTransport.ValidateEndpoint(feed);
        else throw new InvalidDataException("Bearer credentials are available for custom feeds.");
    }

    // Call after checking the configured endpoint. The transport still verifies the actual dial address and TLS.
    internal static string? Resolve(GraphicsFeedDefinition feed, Func<GraphicsFeedDefinition, string?>? resolve)
    {
        if (feed.CredentialReference is null) return null;
        string? bearer;
        try { bearer = resolve?.Invoke(feed); }
        catch (Exception ex) when (ex is not (OperationCanceledException or OutOfMemoryException))
        {
            throw new GraphicsFeedCredentialException();
        }
        return IsValidBearer(bearer) ? bearer : throw new GraphicsFeedCredentialException();
    }
}
