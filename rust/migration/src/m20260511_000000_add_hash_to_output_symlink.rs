use sea_orm_migration::prelude::*;

#[derive(DeriveMigrationName)]
pub struct Migration;

#[async_trait::async_trait]
impl MigrationTrait for Migration {
    async fn up(&self, manager: &SchemaManager) -> Result<(), DbErr> {
        // Add hash of the symlink target string so the client can ingest the
        // target into CAS without recomputing it. Mirrors how output files already carry
        // their content hash.
        manager
            .alter_table(
                Table::alter()
                    .table(OutputSymlink::Table)
                    .add_column(
                        ColumnDef::new(OutputSymlink::Hash)
                            .string()
                            .not_null()
                            .default(""),
                    )
                    .to_owned(),
            )
            .await
    }

    async fn down(&self, manager: &SchemaManager) -> Result<(), DbErr> {
        manager
            .alter_table(
                Table::alter()
                    .table(OutputSymlink::Table)
                    .drop_column(OutputSymlink::Hash)
                    .to_owned(),
            )
            .await
    }
}

#[derive(DeriveIden)]
enum OutputSymlink {
    Table,
    Hash,
}
