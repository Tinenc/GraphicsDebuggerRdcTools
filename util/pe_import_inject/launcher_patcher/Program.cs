// TinecmaTool launcher_main.dll patcher
//
// Replaces the body of KRResources.KRResCheckFlow::Exec with a single call to
// resultCallback.Invoke(true, 0, "...", null) so the WuWa launcher always sees
// the local game resources as intact, even when Client-Win64-ShippingBase.dll
// has been on-disk import-injected.
//
// Usage:
//   LauncherPatcher.exe <input dll> <output dll>
// If args omitted, uses ../launcher_decompile/launcher_main.dll -> launcher_main.patched.dll
// next to the input.

using System;
using System.IO;
using dnlib.DotNet;
using dnlib.DotNet.Emit;
using dnlib.DotNet.Writer;

namespace TinecmaTool.LauncherPatcher;

internal static class Program
{
    private const string PatchedMarker = "TinecmaTool: patched KRResCheckFlow.Exec (skip md5 verify)";

    static int Main(string[] args)
    {
        string inputPath = args.Length >= 1
            ? args[0]
            : Path.GetFullPath(Path.Combine(AppContext.BaseDirectory,
                "..", "..", "..", "..", "launcher_decompile", "launcher_main.dll"));
        string outputPath = args.Length >= 2
            ? args[1]
            : Path.Combine(Path.GetDirectoryName(inputPath)!,
                Path.GetFileNameWithoutExtension(inputPath) + ".patched" +
                Path.GetExtension(inputPath));

        if (!File.Exists(inputPath))
        {
            Console.Error.WriteLine($"[!] input not found: {inputPath}");
            return 2;
        }

        Console.WriteLine($"[*] input  : {inputPath}");
        Console.WriteLine($"[*] output : {outputPath}");

        var module = ModuleDefMD.Load(inputPath);
        try
        {
            int patched = 0;
            patched += PatchResCheckFlow(module) ? 1 : 0;
            // Repair flow stays untouched on purpose: user clicking "立即修复" is
            // the only path that would re-download from CDN, but the UI never
            // shows that button once we make CheckResValid succeed.

            if (patched == 0)
            {
                Console.Error.WriteLine("[!] no methods patched - assembly layout changed?");
                return 3;
            }

            var opts = new ModuleWriterOptions(module)
            {
                MetadataOptions = { Flags = MetadataFlags.PreserveAll },
            };
            module.Write(outputPath, opts);
            Console.WriteLine($"[+] wrote patched assembly ({patched} method(s)).");
            Console.WriteLine($"[+] marker : {PatchedMarker}");
            return 0;
        }
        finally
        {
            module.Dispose();
        }
    }

    // Replace body of:
    //   namespace KRResources
    //   public class KRResCheckFlow
    //     public void Exec(KRUpdateInfo? updateInfo,
    //                      ResCheckProgressCallback progressCallback,
    //                      ResCheckResultCallback resultCallback)
    //
    // New body (semantically):
    //   resultCallback?.Invoke(true, 0, PatchedMarker, null);
    private static bool PatchResCheckFlow(ModuleDefMD module)
    {
        var type = FindType(module, "KRResources", "KRResCheckFlow");
        if (type == null)
        {
            Console.Error.WriteLine("[!] KRResources.KRResCheckFlow not found");
            return false;
        }

        MethodDef? exec = null;
        foreach (var m in type.Methods)
        {
            if (m.Name == "Exec" && m.Parameters.Count == 4 /* this + 3 args */)
            {
                exec = m;
                break;
            }
        }
        if (exec == null)
        {
            Console.Error.WriteLine("[!] KRResCheckFlow.Exec(KRUpdateInfo,..,..) not found");
            return false;
        }

        // Resolve the delegate type (3rd parameter, 0-based index 3 because index 0 is 'this').
        var cbParam = exec.Parameters[3];
        var cbType = cbParam.Type.ToTypeDefOrRef().ResolveTypeDef();
        if (cbType == null)
        {
            Console.Error.WriteLine("[!] could not resolve ResCheckResultCallback delegate type");
            return false;
        }
        MethodDef? invoke = null;
        foreach (var m in cbType.Methods)
        {
            if (m.Name == "Invoke")
            {
                invoke = m;
                break;
            }
        }
        if (invoke == null)
        {
            Console.Error.WriteLine("[!] ResCheckResultCallback.Invoke not found");
            return false;
        }

        Console.WriteLine($"[*] target : {exec.FullName}");
        Console.WriteLine($"[*] invoke : {invoke.FullName}");

        var body = new CilBody();
        body.MaxStack = 8;
        // if (resultCallback == null) return;
        var retInstr = Instruction.Create(OpCodes.Ret);
        body.Instructions.Add(Instruction.Create(OpCodes.Ldarg_3));
        body.Instructions.Add(Instruction.Create(OpCodes.Brfalse_S, retInstr));
        // resultCallback.Invoke(true, 0, marker, null);
        body.Instructions.Add(Instruction.Create(OpCodes.Ldarg_3));
        body.Instructions.Add(Instruction.Create(OpCodes.Ldc_I4_1));   // success = true
        body.Instructions.Add(Instruction.Create(OpCodes.Ldc_I4_0));   // errCode = 0
        body.Instructions.Add(Instruction.Create(OpCodes.Ldstr, PatchedMarker)); // errMessage
        body.Instructions.Add(Instruction.Create(OpCodes.Ldnull));     // path = null
        body.Instructions.Add(Instruction.Create(OpCodes.Callvirt, invoke));
        body.Instructions.Add(retInstr);

        exec.Body = body;
        Console.WriteLine("[+] KRResCheckFlow.Exec body rewritten ({0} instructions)", body.Instructions.Count);
        return true;
    }

    private static TypeDef? FindType(ModuleDefMD module, string ns, string name)
    {
        foreach (var t in module.Types)
        {
            if (t.Namespace == ns && t.Name == name) return t;
        }
        return null;
    }
}
