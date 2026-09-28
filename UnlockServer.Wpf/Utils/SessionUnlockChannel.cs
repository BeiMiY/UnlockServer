using System;
using System.Collections.Concurrent;
using System.Globalization;
using System.IO;
using System.Text;
using System.Threading;

namespace UnlockServer
{
    // 临时指令按目标会话隔离；接收方只读取自己的文件，不能抢走其他锁屏的请求。
    internal sealed class SessionUnlockChannel
    {
        private static readonly ConcurrentDictionary<string, object> Gates = new ConcurrentDictionary<string, object>(StringComparer.OrdinalIgnoreCase);
        private readonly string _directory;
        private readonly uint _sessionId;

        public SessionUnlockChannel(string directory, uint sessionId)
        {
            if (sessionId == uint.MaxValue) throw new ArgumentOutOfRangeException(nameof(sessionId));
            _directory = directory;
            _sessionId = sessionId;
        }

        public string PathFor(string kind) => Path.Combine(_directory,
            "unlock." + _sessionId.ToString(CultureInfo.InvariantCulture) + "." + kind);

        public bool ConsumeClick(DateTime now)
        {
            try
            {
                var path = PathFor("now");
                if (!File.Exists(path)) return false;
                var raw = File.ReadAllText(path).Trim();
                File.Delete(path);
                if (!long.TryParse(raw, NumberStyles.None, CultureInfo.InvariantCulture, out var ticks)) return false;
                return ticks > 0 && ticks <= now.Ticks && now.Ticks - ticks <= TimeSpan.FromSeconds(20).Ticks;
            }
            catch (IOException) { return false; }
            catch (UnauthorizedAccessException) { return false; }
        }

        public bool Request(Action pulse, Action<int> delay = null, int attempts = 50)
        {
            var gate = Gates.GetOrAdd(Path.GetFullPath(PathFor("req")), _ => new object());
            if (!Monitor.TryEnter(gate)) return false;
            try { return RequestCore(pulse, delay ?? Thread.Sleep, attempts); }
            finally { Monitor.Exit(gate); }
        }

        private bool RequestCore(Action pulse, Action<int> delay, int attempts)
        {
            var token = DateTime.UtcNow.Ticks.ToString(CultureInfo.InvariantCulture);
            var request = PathFor("req");
            var result = PathFor("result");
            Directory.CreateDirectory(_directory);
            try
            {
                // 一次请求只写一次。删除文件仅代表已提交，必须等待相同令牌的认证结果。
                File.WriteAllText(request, token, Encoding.ASCII);
                for (int i = 0; i < attempts; i++)
                {
                    pulse();
                    delay(400);
                    string reply;
                    try { reply = File.ReadAllText(result).Trim(); }
                    catch (IOException) { continue; }
                    if (reply == token + "|ok") return true;
                    if (reply == token + "|fail") return false;
                }
                return false;
            }
            finally
            {
                // 超时后撤回自己的请求，避免稍后锁屏组件恢复时意外执行。
                try
                {
                    if (File.Exists(request) && File.ReadAllText(request).Trim() == token)
                        File.Delete(request);
                }
                catch (IOException) { }
                catch (UnauthorizedAccessException) { }
            }
        }
    }
}
