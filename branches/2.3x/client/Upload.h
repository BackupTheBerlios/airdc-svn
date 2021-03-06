#ifndef UPLOAD_H_
#define UPLOAD_H_

#include "forward.h"
#include "Transfer.h"
#include "UploadBundle.h"
#include "Flags.h"
#include "GetSet.h"
#include "Util.h"

namespace dcpp {

class Upload : public Transfer, public Flags {
public:
	enum Flags {
		FLAG_ZUPLOAD = 0x01,
		FLAG_PENDING_KICK = 0x02,
		FLAG_RESUMED = 0x04,
		FLAG_CHUNKED = 0x08,
		FLAG_PARTIAL = 0x10
	};

	bool operator==(const Upload* u) const {
		return compare(getToken(), u->getToken()) == 0;
	}

	Upload(UserConnection& conn, const string& path, const TTHValue& tth);
	~Upload();

	void getParams(const UserConnection& aSource, ParamMap& params) const;

	GETSET(int64_t, fileSize, FileSize);
	GETSET(InputStream*, stream, Stream);
	GETSET(UploadBundlePtr, bundle, Bundle);

	uint8_t delayTime;
};

} // namespace dcpp

#endif /*UPLOAD_H_*/
