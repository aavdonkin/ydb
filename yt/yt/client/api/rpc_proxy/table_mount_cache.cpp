#include "table_mount_cache.h"
#include "api_service_proxy.h"
#include "helpers.h"

#include <yt/yt/client/api/config.h>

#include <yt/yt/client/tablet_client/config.h>

#include <yt/yt/client/tablet_client/table_mount_cache_detail.h>

#include <yt/yt/client/table_client/helpers.h>

namespace NYT::NApi::NRpcProxy {

using namespace NRpc;
using namespace NTableClient;
using namespace NTabletClient;
using namespace NYPath;

using NYT::FromProto;

////////////////////////////////////////////////////////////////////////////////

class TTableMountCache
    : public TTableMountCacheBase
{
public:
    TTableMountCache(
        TTableMountCacheConfigPtr config,
        IChannelPtr channel,
        const NLogging::TLogger& logger,
        TDuration timeout)
        : TTableMountCacheBase(std::move(config), logger)
        , Channel_(std::move(channel))
        , Timeout_(timeout)
    { }

private:
    TFuture<TTableMountInfoPtr> DoGet(const NYPath::TYPath& path, bool /*isPeriodicUpdate*/) noexcept override
    {
        YT_LOG_DEBUG("Requesting table mount info (Path: %v)",
            path);

        TApiServiceProxy proxy(Channel_);
        proxy.SetDefaultTimeout(Timeout_);

        auto req = proxy.GetTableMountInfo();
        req->set_path(path);

        return req->Invoke().Apply(
            BIND([=, this, this_ = MakeStrong(this)] (const TApiServiceProxy::TRspGetTableMountInfoPtr& rsp) {
                auto tableInfo = New<TTableMountInfo>();
                tableInfo->Path = path;
                auto tableId = FromProto<NObjectClient::TObjectId>(rsp->table_id());
                tableInfo->TableId = tableId;

                auto primarySchema = NYT::FromProto<NTableClient::TTableSchemaPtr>(rsp->schema());
                tableInfo->Schemas[ETableSchemaKind::Primary] = primarySchema;
                tableInfo->Schemas[ETableSchemaKind::Write] = primarySchema->ToWrite();
                tableInfo->Schemas[ETableSchemaKind::WriteViaQueueProducer] = primarySchema->ToWriteViaQueueProducer();
                tableInfo->Schemas[ETableSchemaKind::VersionedWrite] = primarySchema->ToVersionedWrite();
                tableInfo->Schemas[ETableSchemaKind::Delete] = primarySchema->ToDelete();
                tableInfo->Schemas[ETableSchemaKind::Query] = primarySchema->ToQuery();
                tableInfo->Schemas[ETableSchemaKind::Lookup] = primarySchema->ToLookup();
                tableInfo->Schemas[ETableSchemaKind::PrimaryWithTabletIndex] = primarySchema->WithTabletIndex();

                tableInfo->UpstreamReplicaId = FromProto<TTableReplicaId>(rsp->upstream_replica_id());
                tableInfo->Dynamic = rsp->dynamic();
                // Non-materialized computed columns are always non-key columns.
                tableInfo->NeedKeyEvaluation = primarySchema->HasMaterializedComputedColumns();

                if (rsp->has_physical_path()) {
                    tableInfo->PhysicalPath = rsp->physical_path();
                } else {
                    tableInfo->PhysicalPath = path;
                }

                tableInfo->Tablets.reserve(rsp->tablets_size());
                for (const auto& protoTabletInfo : rsp->tablets()) {
                    auto tabletInfo = New<NTabletClient::TTabletInfo>();
                    FromProto(tabletInfo.Get(), protoTabletInfo);
                    tabletInfo->TableId = tableId;
                    tabletInfo->UpdateTime = Now();

                    TabletInfoOwnerCache_.Insert(tabletInfo->TabletId, MakeWeak(tableInfo));
                    tableInfo->Tablets.push_back(tabletInfo);
                    if (tabletInfo->State == ETabletState::Mounted) {
                        tableInfo->MountedTablets.push_back(tabletInfo);
                    }
                }

                for (const auto& protoReplicaInfo : rsp->replicas()) {
                    auto replicaInfo = New<TTableReplicaInfo>();
                    replicaInfo->ReplicaId = FromProto<TTableReplicaId>(protoReplicaInfo.replica_id());
                    replicaInfo->ClusterName = protoReplicaInfo.cluster_name();
                    replicaInfo->ReplicaPath = protoReplicaInfo.replica_path();
                    replicaInfo->Mode = FromProto<ETableReplicaMode>(protoReplicaInfo.mode());
                    tableInfo->Replicas.push_back(replicaInfo);
                }

                tableInfo->Indices.reserve(rsp->indices_size());
                for (const auto& protoIndexInfo : rsp->indices()) {
                    auto indexInfo = TIndexInfo{
                        .TableId = FromProto<NObjectClient::TObjectId>(protoIndexInfo.index_table_id()),
                        .Kind = FromProto<ESecondaryIndexKind>(protoIndexInfo.index_kind()),
                        .Predicate = YT_OPTIONAL_FROM_PROTO(protoIndexInfo, predicate),
                        .UnfoldedColumn = YT_OPTIONAL_FROM_PROTO(protoIndexInfo, unfolded_column),
                        .Correspondence = protoIndexInfo.has_index_correspondence()
                            ? FromProto<ETableToIndexCorrespondence>(protoIndexInfo.index_correspondence())
                            : ETableToIndexCorrespondence::Unknown,
                    };

                    if (protoIndexInfo.has_evaluated_columns_schema()) {
                        indexInfo.EvaluatedColumnsSchema = New<TTableSchema>(
                            FromProto<TTableSchema>(protoIndexInfo.evaluated_columns_schema()));
                    }

                    tableInfo->Indices.push_back(std::move(indexInfo));
                }

                if (tableInfo->IsSorted()) {
                    tableInfo->LowerCapBound = MinKey();
                    tableInfo->UpperCapBound = MaxKey();
                } else {
                    tableInfo->LowerCapBound = MakeUnversionedOwningRow(static_cast<int>(0));

                    auto tabletCount = tableInfo->IsChaosReplicated()
                        ? rsp->tablet_count()
                        : std::ssize(tableInfo->Tablets);
                    tableInfo->UpperCapBound = MakeUnversionedOwningRow(tabletCount);
                }

                YT_LOG_DEBUG("Table mount info received (Path: %v, TableId: %v, TabletCount: %v, Dynamic: %v)",
                    path,
                    tableInfo->TableId,
                    tableInfo->Tablets.size(),
                    tableInfo->Dynamic);

                return tableInfo;
            }));
    }

private:
    const IChannelPtr Channel_;
    const TDuration Timeout_;

    void InvalidateTable(const TTableMountInfoPtr& tableInfo) override
    {
        InvalidateValue(tableInfo->Path, tableInfo);
    }
};

////////////////////////////////////////////////////////////////////////////////

ITableMountCachePtr CreateTableMountCache(
    TTableMountCacheConfigPtr config,
    IChannelPtr channel,
    const NLogging::TLogger& logger,
    TDuration timeout)
{
    return New<TTableMountCache>(
        std::move(config),
        std::move(channel),
        logger,
        timeout);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi::NRpcProxy
