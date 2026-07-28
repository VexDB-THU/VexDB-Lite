import SwiftUI

struct ContentView: View {
    private enum HomeTab: Hashable {
        case textExample
        case customText
        case imageExample
        case customImage
    }

    @StateObject private var settings = EmbeddingSettingsModel()
    @State private var selectedTab: HomeTab = {
        let arguments = ProcessInfo.processInfo.arguments
        if arguments.contains("--custom-image-smoke") { return .customImage }
        if arguments.contains("--image-smoke") { return .imageExample }
        if arguments.contains("--user-smoke") { return .customText }
        return .textExample
    }()

    var body: some View {
        TabView(selection: $selectedTab) {
            ExampleDataView(settings: settings)
                .tag(HomeTab.textExample)
                .tabItem {
                    Label("默认文本示例", systemImage: "doc.text.magnifyingglass")
                }

            UserDataView(settings: settings)
                .tag(HomeTab.customText)
                .tabItem {
                    Label("自定义文本", systemImage: "square.and.pencil")
                }

            ImageExampleView()
                .tag(HomeTab.imageExample)
                .tabItem { Label("默认图片示例", systemImage: "photo") }

            CustomImageView()
                .tag(HomeTab.customImage)
                .tabItem { Label("自定义图片", systemImage: "photo.badge.plus") }
        }
        .tint(DemoPalette.cyan)
        .task {
            await ImageAssetSmoke.runIfRequested()
            await ImageIndexIsolationSmoke.runIfRequested()
            await BundledImageSearchSmoke.runIfRequested()
            await BundledTextSmoke.runIfRequested()
            await TextVectorImportSmoke.runIfRequested()
        }
    }
}

#Preview {
    ContentView()
}
