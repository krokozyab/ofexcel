#include "routes.hpp"

#include "ofexcel/json_body.hpp"
#include "ofexcel/migrations.hpp"
#include "ofexcel/version.hpp"

namespace ofexcel {

namespace {

const char *ProfileName(Profile profile) {
	return profile == Profile::LOCAL ? "local" : "hosted";
}

} // namespace

void RegisterHealthRoutes(httplib::Server &server, const RouteContext &context) {
	// GET /v1/health -- CONTRACTS.md §8.3.
	server.Get("/v1/health", [context](const httplib::Request &, httplib::Response &response) {
		auto connection = context.Connect();
		if (!connection) {
			SendError(response, 503, "E_INTERNAL", "База данных закрыта");
			return;
		}
		const auto state = ReadMetaState(*connection);

		JsonObject api;
		api.Set("version", API_VERSION).Set("min_client", API_MIN_CLIENT);
		JsonObject body;
		body.Set("product", "qexcel")
		    .Set("version", PRODUCT_VERSION)
		    .Set("api", api)
		    .Set("profile", ProfileName(context.options.guard.profile))
		    .Set("data_version", state.data_version)
		    .Set("catalog_version", state.catalog_version)
		    .Set("time", UtcNowIso8601());

		SetVersionHeaders(response, *connection);
		SendJson(response, 200, body.Serialize());
	});
}

} // namespace ofexcel
