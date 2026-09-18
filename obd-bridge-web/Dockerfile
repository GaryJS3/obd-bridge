FROM mcr.microsoft.com/dotnet/sdk:10.0 AS build
WORKDIR /src
ARG BUILD_ID
COPY obd-bridge-web/ObdBridge.Web.csproj obd-bridge-web/
RUN dotnet restore obd-bridge-web/ObdBridge.Web.csproj
COPY obd-bridge-web/ obd-bridge-web/
RUN build_id="${BUILD_ID:-$(date -u +%Y%m%dT%H%M%SZ)}" \
    && dotnet publish obd-bridge-web/ObdBridge.Web.csproj -c Release -o /out --no-restore -p:InformationalVersion="$build_id"

FROM mcr.microsoft.com/dotnet/aspnet:10.0 AS runtime
WORKDIR /app
ENV ASPNETCORE_HTTP_PORTS=8080 \
    DATA_PROTECTION_PATH=/data/keys \
    DOTNET_EnableDiagnostics=0
RUN mkdir -p /data/keys && chown -R $APP_UID:$APP_UID /data
COPY --from=build --chown=$APP_UID:$APP_UID /out/ .
USER $APP_UID
VOLUME ["/data"]
EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=3s --start-period=10s --retries=3 CMD ["dotnet", "ObdBridge.Web.dll", "--healthcheck"]
ENTRYPOINT ["dotnet", "ObdBridge.Web.dll"]
