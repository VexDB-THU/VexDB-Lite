#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface VexSearchResult : NSObject
@property(nonatomic, readonly) NSInteger rowID;
@property(nonatomic, copy, readonly) NSString *title;
@property(nonatomic, copy, readonly) NSString *category;
@property(nonatomic, readonly) double distance;
@end

@interface VexDemoSnapshot : NSObject
@property(nonatomic, readonly) BOOL success;
@property(nonatomic, copy, readonly) NSString *message;
@property(nonatomic, copy, readonly) NSString *mode;
@property(nonatomic, copy, readonly) NSString *version;
@property(nonatomic, copy, readonly) NSString *queryPlan;
@property(nonatomic, readonly) NSInteger rowCount;
@property(nonatomic, readonly) long long databaseBytes;
@property(nonatomic, readonly) double buildMilliseconds;
@property(nonatomic, readonly) double queryMilliseconds;
@property(nonatomic, copy, readonly) NSArray<VexSearchResult *> *results;
@end

@interface VexDBBridge : NSObject
@property(nonatomic, copy, readonly) NSString *databasePath;

- (instancetype)initWithDatabasePath:(NSString *)databasePath;
- (VexDemoSnapshot *)rebuildWithMode:(NSString *)mode
    NS_SWIFT_NAME(rebuild(mode:));
- (VexDemoSnapshot *)searchWithPreset:(NSString *)preset limit:(NSInteger)limit
    NS_SWIFT_NAME(search(preset:limit:));
- (VexDemoSnapshot *)reopenAndSearchWithPreset:(NSString *)preset
                                         limit:(NSInteger)limit
    NS_SWIFT_NAME(reopenAndSearch(preset:limit:));
- (VexDemoSnapshot *)importUserChunks:(NSArray<NSString *> *)chunks
                        embeddingData:(NSData *)embeddingData
                           dimensions:(NSInteger)dimensions
    NS_SWIFT_NAME(importUser(chunks:embeddingData:dimensions:));
- (VexDemoSnapshot *)searchUserWithEmbedding:(NSArray<NSNumber *> *)embedding
                                        limit:(NSInteger)limit
    NS_SWIFT_NAME(searchUser(embedding:limit:));
- (VexDemoSnapshot *)userIndexStatus NS_SWIFT_NAME(userIndexStatus());
- (VexDemoSnapshot *)importMediaWithScope:(NSString *)scope
                                   labels:(NSArray<NSString *> *)labels
                               embeddings:(NSArray<NSArray<NSNumber *> *> *)embeddings
    NS_SWIFT_NAME(importMedia(scope:labels:embeddings:));
- (VexDemoSnapshot *)searchMediaWithScope:(NSString *)scope
                                embedding:(NSArray<NSNumber *> *)embedding
                                    limit:(NSInteger)limit
    NS_SWIFT_NAME(searchMedia(scope:embedding:limit:));
- (VexDemoSnapshot *)mediaIndexStatusWithScope:(NSString *)scope
    NS_SWIFT_NAME(mediaIndexStatus(scope:));
- (VexDemoSnapshot *)clearMediaWithScope:(NSString *)scope
    NS_SWIFT_NAME(clearMedia(scope:));
// Compatibility wrappers for the user-owned image index.
- (VexDemoSnapshot *)importUserMedia:(NSArray<NSString *> *)labels
                          embeddings:(NSArray<NSArray<NSNumber *> *> *)embeddings
    NS_SWIFT_NAME(importUserMedia(labels:embeddings:));
- (VexDemoSnapshot *)searchUserMediaWithEmbedding:(NSArray<NSNumber *> *)embedding
                                             limit:(NSInteger)limit
    NS_SWIFT_NAME(searchUserMedia(embedding:limit:));
- (VexDemoSnapshot *)userMediaIndexStatus NS_SWIFT_NAME(userMediaIndexStatus());
@end

NS_ASSUME_NONNULL_END
