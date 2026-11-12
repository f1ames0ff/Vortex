using System.Threading.Tasks;

namespace VortexFomodHost;

/// <summary>
/// Dynamic delegate surface expected by DynamicCoreDelegates. DynamicXDelegates
/// reads these members as function-valued properties, not methods. The host
/// deliberately exposes only the FOMOD context/INI/plugin/UI calls a script may
/// use; game-specific data is intentionally absent in this headless host.
/// </summary>
public sealed class HostSource
{
    public object plugin { get; }
    public object ini { get; }
    public object context { get; }
    public object ui { get; }

    public HostSource()
    {
        plugin = new PluginSource();
        ini = new IniSource();
        context = new ContextSource();
        ui = new UiSource();
    }

    public sealed class PluginSource
    {
        public Func<object?, Task<object>> getAll { get; } =
            _ => Task.FromResult<object>(Array.Empty<string>());
    }

    public sealed class IniSource
    {
        public Func<object[], Task<object>> getIniString { get; } =
            _ => Task.FromResult<object>(string.Empty);

        public Func<object[], Task<object>> getIniInt { get; } =
            _ => Task.FromResult<object>(-1);
    }

    public sealed class ContextSource
    {
        public Func<object?, Task<object>> getAppVersion { get; } =
            _ => Task.FromResult<object>("0.1.0");

        public Func<object?, Task<object>> getCurrentGameVersion { get; } =
            _ => Task.FromResult<object>("1.0.0");

        public Func<object?, Task<object>> getExtenderVersion { get; } =
            _ => Task.FromResult<object>(string.Empty);

        public Func<object?, Task<object>> isExtenderPresent { get; } =
            _ => Task.FromResult<object>(false);

        public Func<object?, Task<object>> checkIfFileExists { get; } =
            _ => Task.FromResult<object>(false);

        public Func<object?, Task<object>> getExistingDataFile { get; } =
            _ => Task.FromResult<object?>(null)!;

        public Func<object[], Task<object>> getExistingDataFileList { get; } =
            _ => Task.FromResult<object>(Array.Empty<string>());
    }

    public sealed class UiSource
    {
        private Action<bool, int>? _continue;

        public Func<object, Task<object>> startDialog { get; }
        public Func<object?, Task<object>> endDialog { get; }
        public Func<object, Task<object>> updateState { get; }
        public Func<object?, Task<object>> reportError { get; }

        public UiSource()
        {
            startDialog = StartDialog;
            endDialog = _ => Task.FromResult<object>(null!);
            updateState = UpdateState;
            reportError = _ => Task.FromResult<object>(null!);
        }

        private Task<object> StartDialog(object parameters)
        {
            _continue = parameters.GetType().GetField("cont")?.GetValue(parameters)
                as Action<bool, int>;
            return Task.FromResult<object>(null!);
        }

        private Task<object> UpdateState(object parameters)
        {
            var cont = _continue;
            if (cont is not null &&
                parameters.GetType().GetField("currentStep")?.GetValue(parameters) is int currentStep)
            {
                _ = Task.Run(() => cont(true, currentStep));
            }
            return Task.FromResult<object>(null!);
        }
    }
}
