/* Minimal indent-aware YAML emitter. betl YAML is simple enough that
 * we don't need a real serializer — string templates with carefully
 * controlled indentation cover everything. */

using System.Text;

namespace Betl.Dtsx2Yaml;

public sealed class YamlWriter
{
    readonly StringBuilder _sb = new();
    int _indent;

    public void Line(string text = "")
    {
        if (text.Length == 0) { _sb.Append('\n'); return; }
        for (int i = 0; i < _indent; ++i) _sb.Append(' ');
        _sb.Append(text);
        _sb.Append('\n');
    }

    public void Comment(string text)
    {
        Line("# " + text);
    }

    public void Indent(int n)      { _indent += n; }

    /* Quote a string for use as a YAML scalar where ambiguity is
     * possible (paths, SQL containing colons, etc.). Single-quoted
     * style with internal '' for embedded quotes — simplest YAML
     * scalar that avoids escape rules. */
    public static string Quote(string s)
    {
        var sb = new StringBuilder(s.Length + 2);
        sb.Append('\'');
        foreach (var c in s) { if (c == '\'') sb.Append("''"); else sb.Append(c); }
        sb.Append('\'');
        return sb.ToString();
    }

    /* Convert an SSIS identifier (containing spaces / special chars
     * etc.) to a betl-friendly step/connection id: lower-snake-case,
     * non-alphanum → _. */
    public static string Id(string raw)
    {
        var sb = new StringBuilder(raw.Length);
        bool wasUnderscore = false;
        foreach (var c in raw)
        {
            if (char.IsLetterOrDigit(c))
            {
                sb.Append(char.ToLowerInvariant(c));
                wasUnderscore = false;
            }
            else
            {
                if (!wasUnderscore) sb.Append('_');
                wasUnderscore = true;
            }
        }
        var s = sb.ToString().Trim('_');
        return s.Length == 0 ? "step" : s;
    }

    public override string ToString() => _sb.ToString();
}
