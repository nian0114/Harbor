import Foundation
import LocalAuthentication

/// Touch ID 门：key.confirm=true 的私钥在用于连接认证前先过
/// deviceOwnerAuthenticationWithBiometrics；拒绝 → 中止连接，
/// secret 不出 Vault。VM 无生物识别：canEvaluate 为 false 时
/// 设置项禁用并说明，运行期回退为确认对话框（同 Linux 语义）。
/// 两个闭包都是注入点，测试可脚本化通过/拒绝。
public enum Biometric {
    public static var canEvaluate: () -> Bool = {
        LAContext().canEvaluatePolicy(
            .deviceOwnerAuthenticationWithBiometrics, error: nil)
    }

    /// canEvaluate 失败时给人看的说明。
    public static var unavailableReason: () -> String = {
        var e: NSError?
        _ = LAContext().canEvaluatePolicy(
            .deviceOwnerAuthenticationWithBiometrics, error: &e)
        if let e { return e.localizedDescription }
        return L("此设备没有可用的生物识别（Touch ID）")
    }

    /// 异步评估；回调切回主线程。
    public static var evaluate:
        (String, @escaping (Bool) -> Void) -> Void = { reason, cb in
            LAContext().evaluatePolicy(
                .deviceOwnerAuthenticationWithBiometrics,
                localizedReason: reason) { ok, _ in
                    DispatchQueue.main.async { cb(ok) }
                }
        }
}
