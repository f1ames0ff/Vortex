using FomodInstaller.ModInstaller;
using VortexFomodHost;

var delegates = new DynamicCoreDelegates(new HostSource());

if (delegates.context.GetAppVersion() != "0.1.0")
{
    throw new InvalidOperationException("context.getAppVersion binding failed");
}
if (delegates.plugin.GetAll(false).Length != 0)
{
    throw new InvalidOperationException("plugin.getAll binding failed");
}
if (delegates.ini.GetIniString("a", "b", "c").Result != string.Empty)
{
    throw new InvalidOperationException("ini.getIniString binding failed");
}
if (delegates.ini.GetIniInt("a", "b", "c").Result != -1)
{
    throw new InvalidOperationException("ini.getIniInt binding failed");
}
if (delegates.context.CheckIfFileExists("missing").Result)
{
    throw new InvalidOperationException("context.checkIfFileExists binding failed");
}
if (delegates.context.GetExistingDataFileList("a", "b", false).Result.Length != 0)
{
    throw new InvalidOperationException("context.getExistingDataFileList binding failed");
}

Console.WriteLine("FOMOD dynamic delegate contract OK");
