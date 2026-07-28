import Foundation

enum DemoDocument {
    static let queries = [
        "自动驾驶汽车如何与道路设施通信？",
        "如何用 Bloom Filter 解决缓存穿透？",
        "智慧农业如何使用无人机和遥感技术？",
        "个人信息保护需要哪些安全技术？",
        "北京的 GDP 是多少？",
        "Smart City 如何预测 traffic congestion？",
        "什么是向量数据库的近似最近邻检索？"
    ]

    static let text: String = {
        guard let url = Bundle.main.url(forResource: "embedding-demo", withExtension: "txt"),
              let value = try? String(contentsOf: url, encoding: .utf8) else {
            return "演示文档读取失败，请重新安装 App。"
        }
        return value
    }()
}
