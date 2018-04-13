/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "passport/passport_form_controller.h"

#include "passport/passport_encryption.h"
#include "passport/passport_panel_controller.h"
#include "boxes/confirm_box.h"
#include "lang/lang_keys.h"
#include "base/openssl_help.h"
#include "base/qthelp_url.h"
#include "mainwindow.h"
#include "window/window_controller.h"
#include "core/click_handler_types.h"
#include "auth_session.h"
#include "storage/localimageloader.h"
#include "storage/localstorage.h"
#include "storage/file_upload.h"
#include "storage/file_download.h"

namespace Passport {
namespace {

constexpr auto kDocumentScansLimit = 20;

QImage ReadImage(bytes::const_span buffer) {
	return App::readImage(QByteArray::fromRawData(
		reinterpret_cast<const char*>(buffer.data()),
		buffer.size()));
}

Value::Type ConvertType(const MTPSecureValueType &type) {
	using Type = Value::Type;
	switch (type.type()) {
	case mtpc_secureValueTypePersonalDetails: return Type::PersonalDetails;
	case mtpc_secureValueTypePassport: return Type::Passport;
	case mtpc_secureValueTypeDriverLicense: return Type::DriverLicense;
	case mtpc_secureValueTypeIdentityCard: return Type::IdentityCard;
	case mtpc_secureValueTypeAddress: return Type::Address;
	case mtpc_secureValueTypeUtilityBill: return Type::UtilityBill;
	case mtpc_secureValueTypeBankStatement: return Type::BankStatement;
	case mtpc_secureValueTypeRentalAgreement: return Type::RentalAgreement;
	case mtpc_secureValueTypePhone: return Type::Phone;
	case mtpc_secureValueTypeEmail: return Type::Email;
	}
	Unexpected("Type in secureValueType type.");
};

MTPSecureValueType ConvertType(Value::Type type) {
	switch (type) {
	case Value::Type::PersonalDetails:
		return MTP_secureValueTypePersonalDetails();
	case Value::Type::Passport:
		return MTP_secureValueTypePassport();
	case Value::Type::DriverLicense:
		return MTP_secureValueTypeDriverLicense();
	case Value::Type::IdentityCard:
		return MTP_secureValueTypeIdentityCard();
	case Value::Type::Address:
		return MTP_secureValueTypeAddress();
	case Value::Type::UtilityBill:
		return MTP_secureValueTypeUtilityBill();
	case Value::Type::BankStatement:
		return MTP_secureValueTypeBankStatement();
	case Value::Type::RentalAgreement:
		return MTP_secureValueTypeRentalAgreement();
	case Value::Type::Phone:
		return MTP_secureValueTypePhone();
	case Value::Type::Email:
		return MTP_secureValueTypeEmail();
	}
	Unexpected("Type in FormController::submit.");
};

QJsonObject GetJSONFromMap(
		const std::map<QString, bytes::const_span> &map) {
	auto result = QJsonObject();
	for (const auto &[key, value] : map) {
		const auto raw = QByteArray::fromRawData(
			reinterpret_cast<const char*>(value.data()),
			value.size());
		result.insert(key, QString::fromUtf8(raw.toBase64()));
	}
	return result;
}

QJsonObject GetJSONFromFile(const File &file) {
	return GetJSONFromMap({
		{ "file_hash", file.hash },
		{ "secret", file.secret }
	});
}

FormRequest PreprocessRequest(const FormRequest &request) {
	auto result = request;
	result.publicKey.replace("\r\n", "\n");
	return result;
}

QString ValueCredentialsKey(Value::Type type) {
	switch (type) {
	case Value::Type::PersonalDetails: return "personal_details";
	case Value::Type::Passport: return "passport";
	case Value::Type::DriverLicense: return "driver_license";
	case Value::Type::IdentityCard: return "identity_card";
	case Value::Type::Address: return "address";
	case Value::Type::UtilityBill: return "utility_bill";
	case Value::Type::BankStatement: return "bank_statement";
	case Value::Type::RentalAgreement: return "rental_agreement";
	case Value::Type::Phone:
	case Value::Type::Email: return QString();
	}
	Unexpected("Type in ValueCredentialsKey.");
}

} // namespace

FormRequest::FormRequest(
	UserId botId,
	const QString &scope,
	const QString &callbackUrl,
	const QString &publicKey,
	const QString &payload)
: botId(botId)
, scope(scope)
, callbackUrl(callbackUrl)
, publicKey(publicKey)
, payload(payload) {
}

EditFile::EditFile(
	not_null<const Value*> value,
	const File &fields,
	std::unique_ptr<UploadScanData> &&uploadData)
: value(value)
, fields(std::move(fields))
, uploadData(std::move(uploadData))
, guard(std::make_shared<bool>(true)) {
}

UploadScanDataPointer::UploadScanDataPointer(
	std::unique_ptr<UploadScanData> &&value)
: _value(std::move(value)) {
}

UploadScanDataPointer::UploadScanDataPointer(
	UploadScanDataPointer &&other) = default;

UploadScanDataPointer &UploadScanDataPointer::operator=(
	UploadScanDataPointer &&other) = default;

UploadScanDataPointer::~UploadScanDataPointer() {
	if (const auto value = _value.get()) {
		if (const auto fullId = value->fullId) {
			Auth().uploader().cancel(fullId);
		}
	}
}

UploadScanData *UploadScanDataPointer::get() const {
	return _value.get();
}

UploadScanDataPointer::operator UploadScanData*() const {
	return _value.get();
}

UploadScanDataPointer::operator bool() const {
	return _value.get();
}

UploadScanData *UploadScanDataPointer::operator->() const {
	return _value.get();
}

Value::Value(Type type) : type(type) {
}

FormController::FormController(
	not_null<Window::Controller*> controller,
	const FormRequest &request)
: _controller(controller)
, _request(PreprocessRequest(request))
, _view(std::make_unique<PanelController>(this)) {
}

void FormController::show() {
	requestForm();
	requestPassword();
}

UserData *FormController::bot() const {
	return _bot;
}

QString FormController::privacyPolicyUrl() const {
	return _form.privacyPolicyUrl;
}

bytes::vector FormController::passwordHashForAuth(
		bytes::const_span password) const {
	return openssl::Sha256(bytes::concatenate(
		_password.salt,
		password,
		_password.salt));
}

auto FormController::prepareFinalData() -> FinalData {
	auto hashes = QVector<MTPSecureValueHash>();
	auto secureData = QJsonObject();
	const auto addValueToJSON = [&](
			const QString &key,
			not_null<const Value*> value) {
		auto object = QJsonObject();
		if (!value->data.parsed.fields.empty()) {
			object.insert("data", GetJSONFromMap({
				{ "data_hash", value->data.hash },
				{ "secret", value->data.secret }
			}));
		}
		if (!value->scans.empty()) {
			auto files = QJsonArray();
			for (const auto &scan : value->scans) {
				files.append(GetJSONFromFile(scan));
			}
			object.insert("files", files);
		}
		if (_form.identitySelfieRequired && value->selfie) {
			object.insert("selfie", GetJSONFromFile(*value->selfie));
		}
		secureData.insert(key, object);
	};
	const auto addValue = [&](not_null<const Value*> value) {
		hashes.push_back(MTP_secureValueHash(
			ConvertType(value->type),
			MTP_bytes(value->submitHash)));
		const auto key = ValueCredentialsKey(value->type);
		if (!key.isEmpty()) {
			addValueToJSON(key, value);
		}
	};
	auto hasErrors = false;
	const auto scopes = ComputeScopes(this);
	for (const auto &scope : scopes) {
		const auto ready = ComputeScopeRowReadyString(scope);
		if (ready.isEmpty()) {
			hasErrors = true;
			findValue(scope.fields)->error = QString();
			continue;
		}
		addValue(scope.fields);
		if (!scope.documents.empty()) {
			for (const auto &document : scope.documents) {
				if (!document->scans.empty()) {
					addValue(document);
					break;
				}
			}
		}
	}
	if (hasErrors) {
		return {};
	}

	auto json = QJsonObject();
	json.insert("secure_data", secureData);
	json.insert("payload", _request.payload);

	return {
		hashes,
		QJsonDocument(json).toJson(QJsonDocument::Compact)
	};
}

bool FormController::submit() {
	if (_submitRequestId) {
		return true;
	}

	const auto prepared = prepareFinalData();
	if (prepared.hashes.empty()) {
		return false;
	}
	const auto credentialsEncryptedData = EncryptData(
		bytes::make_span(prepared.credentials));
	const auto credentialsEncryptedSecret = EncryptCredentialsSecret(
		credentialsEncryptedData.secret,
		bytes::make_span(_request.publicKey.toUtf8()));

	_submitRequestId = request(MTPaccount_AcceptAuthorization(
		MTP_int(_request.botId),
		MTP_string(_request.scope),
		MTP_string(_request.publicKey),
		MTP_vector<MTPSecureValueHash>(prepared.hashes),
		MTP_secureCredentialsEncrypted(
			MTP_bytes(credentialsEncryptedData.bytes),
			MTP_bytes(credentialsEncryptedData.hash),
			MTP_bytes(credentialsEncryptedSecret))
	)).done([=](const MTPBool &result) {
		const auto url = qthelp::url_append_query(
			_request.callbackUrl,
			"tg_passport=success");
		UrlClickHandler::doOpen(url);
	}).fail([=](const RPCError &error) {
		_view->show(Box<InformBox>(
			"Failed sending data :(\n" + error.type()));
	}).send();
	return true;
}

void FormController::submitPassword(const QString &password) {
	Expects(!_password.salt.empty());

	if (_passwordCheckRequestId) {
		return;
	} else if (password.isEmpty()) {
		_passwordError.fire(QString());
	}
	const auto passwordBytes = password.toUtf8();
	_passwordCheckRequestId = request(MTPaccount_GetPasswordSettings(
		MTP_bytes(passwordHashForAuth(bytes::make_span(passwordBytes)))
	)).handleFloodErrors(
	).done([=](const MTPaccount_PasswordSettings &result) {
		Expects(result.type() == mtpc_account_passwordSettings);

		_passwordCheckRequestId = 0;
		const auto &data = result.c_account_passwordSettings();
		_password.confirmedEmail = qs(data.vemail);
		validateSecureSecret(
			bytes::make_span(data.vsecure_salt.v),
			bytes::make_span(data.vsecure_secret.v),
			bytes::make_span(passwordBytes));
	}).fail([=](const RPCError &error) {
		_passwordCheckRequestId = 0;
		if (MTP::isFloodError(error)) {
			_passwordError.fire(lang(lng_flood_error));
		} else if (error.type() == qstr("PASSWORD_HASH_INVALID")) {
			_passwordError.fire(lang(lng_passport_password_wrong));
		} else {
			_passwordError.fire_copy(error.type());
		}
	}).send();
}

void FormController::validateSecureSecret(
		bytes::const_span salt,
		bytes::const_span encryptedSecret,
		bytes::const_span password) {
	if (!salt.empty() && !encryptedSecret.empty()) {
		_secret = DecryptSecureSecret(salt, encryptedSecret, password);
		if (_secret.empty()) {
			_secretId = 0;
			LOG(("API Error: Failed to decrypt secure secret. "
				"Forgetting all files and data :("));
			for (auto &[type, value] : _form.values) {
				if (!value.data.original.isEmpty()) {
					resetValue(value);
				}
			}
		} else {
			_secretId = CountSecureSecretHash(_secret);
			decryptValues();
		}
	}
	if (_secret.empty()) {
		generateSecret(password);
	}
	_secretReady.fire({});
}

void FormController::decryptValues() {
	Expects(!_secret.empty());

	for (auto &[type, value] : _form.values) {
		decryptValue(value);
	}
}

void FormController::decryptValue(Value &value) {
	Expects(!_secret.empty());

	if (!validateValueSecrets(value)) {
		resetValue(value);
		return;
	}
	if (!value.data.original.isEmpty()) {
		value.data.parsed.fields = DeserializeData(DecryptData(
			bytes::make_span(value.data.original),
			value.data.hash,
			value.data.secret));
	}
}

bool FormController::validateValueSecrets(Value &value) {
	if (!value.data.original.isEmpty()) {
		value.data.secret = DecryptValueSecret(
			value.data.encryptedSecret,
			_secret,
			value.data.hash);
		if (value.data.secret.empty()) {
			LOG(("API Error: Could not decrypt data secret. "
				"Forgetting files and data :("));
			return false;
		}
	}
	const auto validateFileSecret = [&](File &file) {
		file.secret = DecryptValueSecret(
			file.encryptedSecret,
			_secret,
			file.hash);
		if (file.secret.empty()) {
			LOG(("API Error: Could not decrypt selfie secret. "
				"Forgetting files and data :("));
			return false;
		}
		return true;
	};
	for (auto &scan : value.scans) {
		if (!validateFileSecret(scan)) {
			return false;
		}
	}
	if (value.selfie && !validateFileSecret(*value.selfie)) {
		return false;
	}
	return true;
}

void FormController::resetValue(Value &value) {
	value = Value(value.type);
}

rpl::producer<QString> FormController::passwordError() const {
	return _passwordError.events();
}

QString FormController::passwordHint() const {
	return _password.hint;
}

void FormController::uploadScan(
		not_null<const Value*> value,
		QByteArray &&content) {
	if (!canAddScan(value)) {
		_view->showToast(lang(lng_passport_scans_limit_reached));
		return;
	}
	const auto nonconst = findValue(value);
	auto scanIndex = int(nonconst->scansInEdit.size());
	nonconst->scansInEdit.emplace_back(
		nonconst,
		File(),
		nullptr);
	auto &scan = nonconst->scansInEdit.back();
	encryptFile(scan, std::move(content), [=](UploadScanData &&result) {
		Expects(scanIndex >= 0 && scanIndex < nonconst->scansInEdit.size());

		uploadEncryptedFile(
			nonconst->scansInEdit[scanIndex],
			std::move(result));
	});
}

void FormController::deleteScan(
		not_null<const Value*> value,
		int scanIndex) {
	scanDeleteRestore(value, scanIndex, true);
}

void FormController::restoreScan(
		not_null<const Value*> value,
		int scanIndex) {
	scanDeleteRestore(value, scanIndex, false);
}

void FormController::uploadSelfie(
		not_null<const Value*> value,
		QByteArray &&content) {
	const auto nonconst = findValue(value);
	nonconst->selfieInEdit = EditFile{ nonconst, File(), nullptr };
	auto &file = *nonconst->selfieInEdit;
	encryptFile(file, std::move(content), [=](UploadScanData &&result) {
		uploadEncryptedFile(
			*nonconst->selfieInEdit,
			std::move(result));
	});
}

void FormController::deleteSelfie(not_null<const Value*> value) {
	selfieDeleteRestore(value, true);
}

void FormController::restoreSelfie(not_null<const Value*> value) {
	selfieDeleteRestore(value, false);
}

void FormController::prepareFile(
		EditFile &file,
		const QByteArray &content) {
	const auto fileId = rand_value<uint64>();
	file.fields.size = content.size();
	file.fields.id = fileId;
	file.fields.dcId = MTP::maindc();
	file.fields.secret = GenerateSecretBytes();
	file.fields.date = unixtime();
	file.fields.image = ReadImage(bytes::make_span(content));
	file.fields.downloadOffset = file.fields.size;

	_scanUpdated.fire(&file);
}

void FormController::encryptFile(
		EditFile &file,
		QByteArray &&content,
		base::lambda<void(UploadScanData &&result)> callback) {
	prepareFile(file, content);

	const auto weak = std::weak_ptr<bool>(file.guard);
	crl::async([
		=,
		fileId = file.fields.id,
		bytes = std::move(content),
		fileSecret = file.fields.secret
	] {
		auto data = EncryptData(
			bytes::make_span(bytes),
			fileSecret);
		auto result = UploadScanData();
		result.fileId = fileId;
		result.hash = std::move(data.hash);
		result.bytes = std::move(data.bytes);
		result.md5checksum.resize(32);
		hashMd5Hex(
			result.bytes.data(),
			result.bytes.size(),
			result.md5checksum.data());
		crl::on_main([=, encrypted = std::move(result)]() mutable {
			if (weak.lock()) {
				callback(std::move(encrypted));
			}
		});
	});
}

void FormController::scanDeleteRestore(
		not_null<const Value*> value,
		int scanIndex,
		bool deleted) {
	Expects(scanIndex >= 0 && scanIndex < value->scansInEdit.size());

	const auto nonconst = findValue(value);
	auto &scan = nonconst->scansInEdit[scanIndex];
	if (scan.deleted && !deleted) {
		if (!canAddScan(value)) {
			_view->showToast(lang(lng_passport_scans_limit_reached));
			return;
		}
	}
	scan.deleted = deleted;
	_scanUpdated.fire(&scan);
}

void FormController::selfieDeleteRestore(
		not_null<const Value*> value,
		bool deleted) {
	Expects(value->selfieInEdit.has_value());

	const auto nonconst = findValue(value);
	auto &scan = *nonconst->selfieInEdit;
	scan.deleted = deleted;
	_scanUpdated.fire(&scan);
}

bool FormController::canAddScan(not_null<const Value*> value) const {
	const auto scansCount = ranges::count_if(
		value->scansInEdit,
		[](const EditFile &scan) { return !scan.deleted; });
	return (scansCount < kDocumentScansLimit);
}

void FormController::subscribeToUploader() {
	if (_uploaderSubscriptions) {
		return;
	}

	using namespace Storage;

	Auth().uploader().secureReady(
	) | rpl::start_with_next([=](const UploadSecureDone &data) {
		scanUploadDone(data);
	}, _uploaderSubscriptions);

	Auth().uploader().secureProgress(
	) | rpl::start_with_next([=](const UploadSecureProgress &data) {
		scanUploadProgress(data);
	}, _uploaderSubscriptions);

	Auth().uploader().secureFailed(
	) | rpl::start_with_next([=](const FullMsgId &fullId) {
		scanUploadFail(fullId);
	}, _uploaderSubscriptions);
}

void FormController::uploadEncryptedFile(
		EditFile &file,
		UploadScanData &&data) {
	subscribeToUploader();

	file.uploadData = std::make_unique<UploadScanData>(std::move(data));

	auto prepared = std::make_shared<FileLoadResult>(
		TaskId(),
		file.uploadData->fileId,
		FileLoadTo(PeerId(0), false, MsgId(0)),
		TextWithTags(),
		std::shared_ptr<SendingAlbum>(nullptr));
	prepared->type = SendMediaType::Secure;
	prepared->content = QByteArray::fromRawData(
		reinterpret_cast<char*>(file.uploadData->bytes.data()),
		file.uploadData->bytes.size());
	prepared->setFileData(prepared->content);
	prepared->filemd5 = file.uploadData->md5checksum;

	file.uploadData->fullId = FullMsgId(0, clientMsgId());
	Auth().uploader().upload(file.uploadData->fullId, std::move(prepared));
}

void FormController::scanUploadDone(const Storage::UploadSecureDone &data) {
	if (const auto file = findEditFile(data.fullId)) {
		Assert(file->uploadData != nullptr);
		Assert(file->uploadData->fileId == data.fileId);

		file->uploadData->partsCount = data.partsCount;
		file->fields.hash = std::move(file->uploadData->hash);
		file->fields.encryptedSecret = EncryptValueSecret(
			file->fields.secret,
			_secret,
			file->fields.hash);
		file->uploadData->fullId = FullMsgId();

		_scanUpdated.fire(file);
	}
}

void FormController::scanUploadProgress(
		const Storage::UploadSecureProgress &data) {
	if (const auto file = findEditFile(data.fullId)) {
		Assert(file->uploadData != nullptr);

		file->uploadData->offset = data.offset;

		_scanUpdated.fire(file);
	}
}

void FormController::scanUploadFail(const FullMsgId &fullId) {
	if (const auto file = findEditFile(fullId)) {
		Assert(file->uploadData != nullptr);

		file->uploadData->offset = -1;

		_scanUpdated.fire(file);
	}
}

rpl::producer<> FormController::secretReadyEvents() const {
	return _secretReady.events();
}

QString FormController::defaultEmail() const {
	return _password.confirmedEmail;
}

QString FormController::defaultPhoneNumber() const {
	if (const auto self = App::self()) {
		return self->phone();
	}
	return QString();
}

auto FormController::scanUpdated() const
-> rpl::producer<not_null<const EditFile*>> {
	return _scanUpdated.events();
}

auto FormController::valueSaveFinished() const
-> rpl::producer<not_null<const Value*>> {
	return _valueSaveFinished.events();
}

auto FormController::verificationNeeded() const
-> rpl::producer<not_null<const Value*>> {
	return _verificationNeeded.events();
}

auto FormController::verificationUpdate() const
-> rpl::producer<not_null<const Value*>> {
	return _verificationUpdate.events();
}

void FormController::verify(
		not_null<const Value*> value,
		const QString &code) {
	if (value->verification.requestId) {
		return;
	}
	const auto nonconst = findValue(value);
	const auto prepared = code.trimmed();
	Assert(nonconst->verification.codeLength != 0);
	verificationError(nonconst, QString());
	if (nonconst->verification.codeLength > 0
		&& nonconst->verification.codeLength != prepared.size()) {
		verificationError(nonconst, lang(lng_signin_wrong_code));
		return;
	} else if (prepared.isEmpty()) {
		verificationError(nonconst, lang(lng_signin_wrong_code));
		return;
	}
	nonconst->verification.requestId = [&] {
		switch (nonconst->type) {
		case Value::Type::Phone:
			return request(MTPaccount_VerifyPhone(
				MTP_string(getPhoneFromValue(nonconst)),
				MTP_string(nonconst->verification.phoneCodeHash),
				MTP_string(prepared)
			)).done([=](const MTPBool &result) {
				savePlainTextValue(nonconst);
				clearValueVerification(nonconst);
			}).fail([=](const RPCError &error) {
				nonconst->verification.requestId = 0;
				if (error.type() == qstr("PHONE_CODE_INVALID")) {
					verificationError(
						nonconst,
						lang(lng_signin_wrong_code));
				} else {
					verificationError(nonconst, error.type());
				}
			}).send();
		case Value::Type::Email:
			return request(MTPaccount_VerifyEmail(
				MTP_string(getEmailFromValue(nonconst)),
				MTP_string(prepared)
			)).done([=](const MTPBool &result) {
				savePlainTextValue(nonconst);
				clearValueVerification(nonconst);
			}).fail([=](const RPCError &error) {
				nonconst->verification.requestId = 0;
				if (error.type() == qstr("CODE_INVALID")) {
					verificationError(
						nonconst,
						lang(lng_signin_wrong_code));
				} else {
					verificationError(nonconst, error.type());
				}
			}).send();
		}
		Unexpected("Type in FormController::verify().");
	}();
}

void FormController::verificationError(
		not_null<Value*> value,
		const QString &text) {
	value->verification.error = text;
	_verificationUpdate.fire_copy(value);
}

const Form &FormController::form() const {
	return _form;
}

not_null<Value*> FormController::findValue(not_null<const Value*> value) {
	const auto i = _form.values.find(value->type);
	Assert(i != end(_form.values));
	const auto result = &i->second;

	Ensures(result == value);
	return result;
}

void FormController::startValueEdit(not_null<const Value*> value) {
	const auto nonconst = findValue(value);
	++nonconst->editScreens;
	if (savingValue(nonconst)) {
		return;
	}
	for (auto &scan : nonconst->scans) {
		loadFile(scan);
	}
	if (nonconst->selfie) {
		loadFile(*nonconst->selfie);
	}
	nonconst->scansInEdit = ranges::view::all(
		nonconst->scans
	) | ranges::view::transform([=](const File &file) {
		return EditFile(nonconst, file, nullptr);
	}) | ranges::to_vector;

	if (nonconst->selfie) {
		nonconst->selfieInEdit = EditFile(
			nonconst,
			*nonconst->selfie,
			nullptr);
	} else {
		nonconst->selfieInEdit = base::none;
	}

	nonconst->data.parsedInEdit = nonconst->data.parsed;
}

void FormController::loadFile(File &file) {
	if (!file.image.isNull()) {
		file.downloadOffset = file.size;
		return;
	}

	const auto key = FileKey{ file.id, file.dcId };
	const auto i = _fileLoaders.find(key);
	if (i != _fileLoaders.end()) {
		return;
	}
	file.downloadOffset = 0;
	const auto [j, ok] = _fileLoaders.emplace(
		key,
		std::make_unique<mtpFileLoader>(
			file.dcId,
			file.id,
			file.accessHash,
			0,
			SecureFileLocation,
			QString(),
			file.size,
			LoadToCacheAsWell,
			LoadFromCloudOrLocal,
			false));
	const auto loader = j->second.get();
	loader->connect(loader, &mtpFileLoader::progress, [=] {
		if (loader->finished()) {
			fileLoadDone(key, loader->bytes());
		} else {
			fileLoadProgress(key, loader->currentOffset());
		}
	});
	loader->connect(loader, &mtpFileLoader::failed, [=] {
		fileLoadFail(key);
	});
	loader->start();
}

void FormController::fileLoadDone(FileKey key, const QByteArray &bytes) {
	if (const auto [value, file] = findFile(key); file != nullptr) {
		const auto decrypted = DecryptData(
			bytes::make_span(bytes),
			file->hash,
			file->secret);
		if (decrypted.empty()) {
			fileLoadFail(key);
			return;
		}
		file->downloadOffset = file->size;
		file->image = App::readImage(QByteArray::fromRawData(
			reinterpret_cast<const char*>(decrypted.data()),
			decrypted.size()));
		if (const auto fileInEdit = findEditFile(key)) {
			fileInEdit->fields.image = file->image;
			fileInEdit->fields.downloadOffset = file->downloadOffset;
			_scanUpdated.fire(fileInEdit);
		}
	}
}

void FormController::fileLoadProgress(FileKey key, int offset) {
	if (const auto [value, file] = findFile(key); file != nullptr) {
		file->downloadOffset = offset;
		if (const auto fileInEdit = findEditFile(key)) {
			fileInEdit->fields.downloadOffset = file->downloadOffset;
			_scanUpdated.fire(fileInEdit);
		}
	}
}

void FormController::fileLoadFail(FileKey key) {
	if (const auto [value, file] = findFile(key); file != nullptr) {
		file->downloadOffset = -1;
		if (const auto fileInEdit = findEditFile(key)) {
			fileInEdit->fields.downloadOffset = file->downloadOffset;
			_scanUpdated.fire(fileInEdit);
		}
	}
}

bool FormController::savingValue(not_null<const Value*> value) const {
	return (value->saveRequestId != 0)
		|| (value->verification.requestId != 0)
		|| (value->verification.codeLength != 0);
}

void FormController::cancelValueEdit(not_null<const Value*> value) {
	Expects(value->editScreens > 0);

	const auto nonconst = findValue(value);
	--nonconst->editScreens;
	clearValueEdit(nonconst);
}

void FormController::valueEditFailed(not_null<Value*> value) {
	Expects(!savingValue(value));

	if (value->editScreens == 0) {
		clearValueEdit(value);
	}
}

void FormController::clearValueEdit(not_null<Value*> value) {
	if (savingValue(value)) {
		return;
	}
	value->scansInEdit.clear();
	value->selfieInEdit = base::none;
	value->data.encryptedSecretInEdit.clear();
	value->data.hashInEdit.clear();
	value->data.parsedInEdit = ValueMap();
}

void FormController::cancelValueVerification(not_null<const Value*> value) {
	const auto nonconst = findValue(value);
	clearValueVerification(nonconst);
	if (!savingValue(nonconst)) {
		valueEditFailed(nonconst);
	}
}

void FormController::clearValueVerification(not_null<Value*> value) {
	const auto was = (value->verification.codeLength != 0);
	if (const auto requestId = base::take(value->verification.requestId)) {
		request(requestId).cancel();
	}
	value->verification = Verification();
	if (was) {
		_verificationUpdate.fire_copy(value);
	}
}

bool FormController::isEncryptedValue(Value::Type type) const {
	return (type != Value::Type::Phone && type != Value::Type::Email);
}

bool FormController::editFileChanged(const EditFile &file) const {
	if (file.uploadData) {
		return !file.deleted;
	}
	return file.deleted;
}

bool FormController::editValueChanged(
		not_null<const Value*> value,
		const ValueMap &data) const {
	auto filesCount = 0;
	for (const auto &scan : value->scansInEdit) {
		if (editFileChanged(scan)) {
			return true;
		}
	}
	if (value->selfieInEdit && editFileChanged(*value->selfieInEdit)) {
		return true;
	}
	auto existing = value->data.parsed.fields;
	for (const auto &[key, value] : data.fields) {
		const auto i = existing.find(key);
		if (i != existing.end()) {
			if (i->second != value) {
				return true;
			}
			existing.erase(i);
		} else if (!value.isEmpty()) {
			return true;
		}
	}
	return !existing.empty();
}

void FormController::saveValueEdit(
		not_null<const Value*> value,
		ValueMap &&data) {
	if (savingValue(value) || _submitRequestId) {
		return;
	}

	const auto nonconst = findValue(value);
	if (!editValueChanged(nonconst, data)) {
		nonconst->saveRequestId = -1;
		crl::on_main(this, [=] {
			base::take(nonconst->scansInEdit);
			base::take(nonconst->selfieInEdit);
			base::take(nonconst->data.encryptedSecretInEdit);
			base::take(nonconst->data.hashInEdit);
			base::take(nonconst->data.parsedInEdit);
			base::take(nonconst->error);
			nonconst->saveRequestId = 0;
			_valueSaveFinished.fire_copy(nonconst);
		});
		return;
	}
	nonconst->data.parsedInEdit = std::move(data);

	if (isEncryptedValue(nonconst->type)) {
		saveEncryptedValue(nonconst);
	} else {
		savePlainTextValue(nonconst);
	}
}

void FormController::deleteValueEdit(not_null<const Value*> value) {
	if (savingValue(value) || _submitRequestId) {
		return;
	}

	const auto nonconst = findValue(value);
	nonconst->saveRequestId = request(MTPaccount_DeleteSecureValue(
		MTP_vector<MTPSecureValueType>(1, ConvertType(nonconst->type))
	)).done([=](const MTPBool &result) {
		const auto editScreens = value->editScreens;
		*nonconst = Value(nonconst->type);
		nonconst->editScreens = editScreens;

		_valueSaveFinished.fire_copy(value);
	}).fail([=](const RPCError &error) {
		nonconst->saveRequestId = 0;
		valueSaveFailed(nonconst, error);
	}).send();
}

void FormController::saveEncryptedValue(not_null<Value*> value) {
	Expects(isEncryptedValue(value->type));

	if (_secret.empty()) {
		_secretCallbacks.push_back([=] {
			saveEncryptedValue(value);
		});
		return;
	}

	const auto inputFile = [](const EditFile &file) {
		if (const auto uploadData = file.uploadData.get()) {
			return MTP_inputSecureFileUploaded(
				MTP_long(file.fields.id),
				MTP_int(uploadData->partsCount),
				MTP_bytes(uploadData->md5checksum),
				MTP_bytes(file.fields.hash),
				MTP_bytes(file.fields.encryptedSecret));
		}
		return MTP_inputSecureFile(
			MTP_long(file.fields.id),
			MTP_long(file.fields.accessHash));
	};

	auto inputFiles = QVector<MTPInputSecureFile>();
	inputFiles.reserve(value->scansInEdit.size());
	for (const auto &scan : value->scansInEdit) {
		if (scan.deleted) {
			continue;
		}
		inputFiles.push_back(inputFile(scan));
	}

	if (value->data.secret.empty()) {
		value->data.secret = GenerateSecretBytes();
	}
	const auto encryptedData = EncryptData(
		SerializeData(value->data.parsedInEdit.fields),
		value->data.secret);
	value->data.hashInEdit = encryptedData.hash;
	value->data.encryptedSecretInEdit = EncryptValueSecret(
		value->data.secret,
		_secret,
		value->data.hashInEdit);

	const auto selfie = (value->selfieInEdit
		&& !value->selfieInEdit->deleted)
		? inputFile(*value->selfieInEdit)
		: MTPInputSecureFile();

	const auto type = [&] {
		switch (value->type) {
		case Value::Type::PersonalDetails:
			return MTP_secureValueTypePersonalDetails();
		case Value::Type::Passport:
			return MTP_secureValueTypePassport();
		case Value::Type::DriverLicense:
			return MTP_secureValueTypeDriverLicense();
		case Value::Type::IdentityCard:
			return MTP_secureValueTypeIdentityCard();
		case Value::Type::Address:
			return MTP_secureValueTypeAddress();
		case Value::Type::UtilityBill:
			return MTP_secureValueTypeUtilityBill();
		case Value::Type::BankStatement:
			return MTP_secureValueTypeBankStatement();
		case Value::Type::RentalAgreement:
			return MTP_secureValueTypeRentalAgreement();
		}
		Unexpected("Value type in saveEncryptedValue().");
	}();
	const auto flags = ((value->scansInEdit.empty()
		&& value->data.parsedInEdit.fields.empty())
			? MTPDinputSecureValue::Flag(0)
			: MTPDinputSecureValue::Flag::f_data)
		| (value->scansInEdit.empty()
			? MTPDinputSecureValue::Flag(0)
			: MTPDinputSecureValue::Flag::f_files)
		| ((value->selfieInEdit && !value->selfieInEdit->deleted)
			? MTPDinputSecureValue::Flag::f_selfie
			: MTPDinputSecureValue::Flag(0));
	Assert(flags != MTPDinputSecureValue::Flags(0));

	sendSaveRequest(value, MTP_inputSecureValue(
		MTP_flags(flags),
		type,
		MTP_secureData(
			MTP_bytes(encryptedData.bytes),
			MTP_bytes(value->data.hashInEdit),
			MTP_bytes(value->data.encryptedSecretInEdit)),
		MTP_vector<MTPInputSecureFile>(inputFiles),
		MTPSecurePlainData(),
		selfie));
}

void FormController::savePlainTextValue(not_null<Value*> value) {
	Expects(!isEncryptedValue(value->type));

	const auto text = getPlainTextFromValue(value);
	const auto type = [&] {
		switch (value->type) {
		case Value::Type::Phone: return MTP_secureValueTypePhone();
		case Value::Type::Email: return MTP_secureValueTypeEmail();
		}
		Unexpected("Value type in savePlainTextValue().");
	}();
	const auto plain = [&] {
		switch (value->type) {
		case Value::Type::Phone: return MTP_securePlainPhone;
		case Value::Type::Email: return MTP_securePlainEmail;
		}
		Unexpected("Value type in savePlainTextValue().");
	}();
	sendSaveRequest(value, MTP_inputSecureValue(
		MTP_flags(MTPDinputSecureValue::Flag::f_plain_data),
		type,
		MTPSecureData(),
		MTPVector<MTPInputSecureFile>(),
		plain(MTP_string(text)),
		MTPInputSecureFile()));
}

void FormController::sendSaveRequest(
		not_null<Value*> value,
		const MTPInputSecureValue &data) {
	Expects(value->saveRequestId == 0);

	value->saveRequestId = request(MTPaccount_SaveSecureValue(
		data,
		MTP_long(_secretId)
	)).done([=](const MTPSecureValue &result) {
		auto filesInEdit = base::take(value->scansInEdit);
		if (auto selfie = base::take(value->selfieInEdit)) {
			filesInEdit.push_back(std::move(*selfie));
		}

		const auto editScreens = value->editScreens;
		*value = parseValue(result, filesInEdit);
		decryptValue(*value);
		value->editScreens = editScreens;

		_valueSaveFinished.fire_copy(value);
	}).fail([=](const RPCError &error) {
		value->saveRequestId = 0;
		if (error.type() == qstr("PHONE_VERIFICATION_NEEDED")) {
			if (value->type == Value::Type::Phone) {
				startPhoneVerification(value);
				return;
			}
		} else if (error.type() == qstr("EMAIL_VERIFICATION_NEEDED")) {
			if (value->type == Value::Type::Email) {
				startEmailVerification(value);
				return;
			}
		}
		valueSaveFailed(value, error);
	}).send();
}

QString FormController::getPhoneFromValue(
		not_null<const Value*> value) const {
	Expects(value->type == Value::Type::Phone);

	return getPlainTextFromValue(value);
}

QString FormController::getEmailFromValue(
		not_null<const Value*> value) const {
	Expects(value->type == Value::Type::Email);

	return getPlainTextFromValue(value);
}

QString FormController::getPlainTextFromValue(
		not_null<const Value*> value) const {
	Expects(value->type == Value::Type::Phone
		|| value->type == Value::Type::Email);

	const auto i = value->data.parsedInEdit.fields.find("value");
	Assert(i != end(value->data.parsedInEdit.fields));
	return i->second;
}

void FormController::startPhoneVerification(not_null<Value*> value) {
	value->verification.requestId = request(MTPaccount_SendVerifyPhoneCode(
		MTP_flags(MTPaccount_SendVerifyPhoneCode::Flag(0)),
		MTP_string(getPhoneFromValue(value)),
		MTPBool()
	)).done([=](const MTPauth_SentCode &result) {
		Expects(result.type() == mtpc_auth_sentCode);

		value->verification.requestId = 0;

		const auto &data = result.c_auth_sentCode();
		value->verification.phoneCodeHash = qs(data.vphone_code_hash);
		switch (data.vtype.type()) {
		case mtpc_auth_sentCodeTypeApp:
			LOG(("API Error: sentCodeTypeApp not expected "
				"in FormController::startPhoneVerification."));
			return;
		case mtpc_auth_sentCodeTypeFlashCall:
			LOG(("API Error: sentCodeTypeFlashCall not expected "
				"in FormController::startPhoneVerification."));
			return;
		case mtpc_auth_sentCodeTypeCall: {
			const auto &type = data.vtype.c_auth_sentCodeTypeCall();
			value->verification.codeLength = (type.vlength.v > 0)
				? type.vlength.v
				: -1;
			value->verification.call = std::make_unique<SentCodeCall>(
				[=] { requestPhoneCall(value); },
				[=] { _verificationUpdate.fire_copy(value); });
			value->verification.call->setStatus(
				{ SentCodeCall::State::Called, 0 });
			if (data.has_next_type()) {
				LOG(("API Error: next_type is not supported for calls."));
			}
		} break;
		case mtpc_auth_sentCodeTypeSms: {
			const auto &type = data.vtype.c_auth_sentCodeTypeSms();
			value->verification.codeLength = (type.vlength.v > 0)
				? type.vlength.v
				: -1;
			const auto &next = data.vnext_type;
			if (data.has_next_type()
				&& next.type() == mtpc_auth_codeTypeCall) {
				value->verification.call = std::make_unique<SentCodeCall>(
					[=] { requestPhoneCall(value); },
					[=] { _verificationUpdate.fire_copy(value); });
				value->verification.call->setStatus({
					SentCodeCall::State::Waiting,
					data.has_timeout() ? data.vtimeout.v : 60 });
			}
		} break;
		}
		_verificationNeeded.fire_copy(value);
	}).fail([=](const RPCError &error) {
		value->verification.requestId = 0;
		valueSaveFailed(value, error);
	}).send();
}

void FormController::startEmailVerification(not_null<Value*> value) {
	value->verification.requestId = request(MTPaccount_SendVerifyEmailCode(
		MTP_string(getEmailFromValue(value))
	)).done([=](const MTPaccount_SentEmailCode &result) {
		Expects(result.type() == mtpc_account_sentEmailCode);

		value->verification.requestId = 0;
		const auto &data = result.c_account_sentEmailCode();
		value->verification.codeLength = (data.vlength.v > 0)
			? data.vlength.v
			: -1;
		_verificationNeeded.fire_copy(value);
	}).fail([=](const RPCError &error) {
		valueSaveFailed(value, error);
	}).send();
}


void FormController::requestPhoneCall(not_null<Value*> value) {
	Expects(value->verification.call != nullptr);

	value->verification.call->setStatus(
		{ SentCodeCall::State::Calling, 0 });
	request(MTPauth_ResendCode(
		MTP_string(getPhoneFromValue(value)),
		MTP_string(value->verification.phoneCodeHash)
	)).done([=](const MTPauth_SentCode &code) {
		value->verification.call->callDone();
	}).send();
}

void FormController::valueSaveFailed(
		not_null<Value*> value,
		const RPCError &error) {
	_view->show(Box<InformBox>("Error saving value:\n" + error.type()));
	valueEditFailed(value);
	_valueSaveFinished.fire_copy(value);
}

void FormController::generateSecret(bytes::const_span password) {
	if (_saveSecretRequestId) {
		return;
	}
	auto secret = GenerateSecretBytes();

	auto randomSaltPart = bytes::vector(8);
	bytes::set_random(randomSaltPart);
	auto newSecureSaltFull = bytes::concatenate(
		_password.newSecureSalt,
		randomSaltPart);

	auto secureSecretId = CountSecureSecretHash(secret);
	auto encryptedSecret = EncryptSecureSecret(
		newSecureSaltFull,
		secret,
		password);

	const auto hashForAuth = openssl::Sha256(bytes::concatenate(
		_password.salt,
		password,
		_password.salt));

	using Flag = MTPDaccount_passwordInputSettings::Flag;
	_saveSecretRequestId = request(MTPaccount_UpdatePasswordSettings(
		MTP_bytes(hashForAuth),
		MTP_account_passwordInputSettings(
			MTP_flags(Flag::f_new_secure_secret),
			MTPbytes(), // new_salt
			MTPbytes(), // new_password_hash
			MTPstring(), // hint
			MTPstring(), // email
			MTP_bytes(newSecureSaltFull),
			MTP_bytes(encryptedSecret),
			MTP_long(secureSecretId))
	)).done([=](const MTPBool &result) {
		_saveSecretRequestId = 0;
		_secret = secret;
		_secretId = secureSecretId;
		//_password.salt = newPasswordSaltFull;
		for (const auto &callback : base::take(_secretCallbacks)) {
			callback();
		}
	}).fail([=](const RPCError &error) {
		// #TODO wrong password hash error?
		Ui::show(Box<InformBox>("Saving encrypted value failed."));
		_saveSecretRequestId = 0;
	}).send();
}

void FormController::requestForm() {
	if (_request.payload.isEmpty()) {
		_formRequestId = -1;
		Ui::show(Box<InformBox>(lang(lng_passport_form_error)));
		return;
	}
	_formRequestId = request(MTPaccount_GetAuthorizationForm(
		MTP_int(_request.botId),
		MTP_string(_request.scope),
		MTP_string(_request.publicKey)
	)).done([=](const MTPaccount_AuthorizationForm &result) {
		_formRequestId = 0;
		formDone(result);
	}).fail([=](const RPCError &error) {
		formFail(error);
	}).send();
}

auto FormController::parseFiles(
	const QVector<MTPSecureFile> &data,
	const std::vector<EditFile> &editData) const
-> std::vector<File> {
	auto result = std::vector<File>();
	result.reserve(data.size());

	for (const auto &file : data) {
		if (auto normal = parseFile(file, editData)) {
			result.push_back(std::move(*normal));
		}
	}

	return result;
}

auto FormController::parseFile(
	const MTPSecureFile &data,
	const std::vector<EditFile> &editData) const
-> base::optional<File> {
	switch (data.type()) {
	case mtpc_secureFileEmpty:
		return base::none;

	case mtpc_secureFile: {
		const auto &fields = data.c_secureFile();
		auto result = File();
		result.id = fields.vid.v;
		result.accessHash = fields.vaccess_hash.v;
		result.size = fields.vsize.v;
		result.date = fields.vdate.v;
		result.dcId = fields.vdc_id.v;
		result.hash = bytes::make_vector(fields.vfile_hash.v);
		result.encryptedSecret = bytes::make_vector(fields.vsecret.v);
		fillDownloadedFile(result, editData);
		return result;
	} break;
	}
	Unexpected("Type in FormController::parseFile.");
}

void FormController::fillDownloadedFile(
		File &destination,
		const std::vector<EditFile> &source) const {
	const auto i = ranges::find(
		source,
		destination.hash,
		[](const EditFile &file) { return file.fields.hash; });
	if (i == source.end()) {
		return;
	}
	destination.image = i->fields.image;
	destination.downloadOffset = i->fields.downloadOffset;
	if (!i->uploadData) {
		return;
	}
	Local::writeImage(
		StorageKey(
			storageMix32To64(
				SecureFileLocation,
				destination.dcId),
			destination.id),
		StorageImageSaved(QByteArray::fromRawData(
			reinterpret_cast<const char*>(
				i->uploadData->bytes.data()),
			i->uploadData->bytes.size())));
}

auto FormController::parseValue(
		const MTPSecureValue &value,
		const std::vector<EditFile> &editData) const -> Value {
	Expects(value.type() == mtpc_secureValue);

	const auto &data = value.c_secureValue();
	const auto type = ConvertType(data.vtype);
	auto result = Value(type);
	result.submitHash = bytes::make_vector(data.vhash.v);
	if (data.has_data()) {
		Assert(data.vdata.type() == mtpc_secureData);
		const auto &fields = data.vdata.c_secureData();
		result.data.original = fields.vdata.v;
		result.data.hash = bytes::make_vector(fields.vdata_hash.v);
		result.data.encryptedSecret = bytes::make_vector(fields.vsecret.v);
	}
	if (data.has_files()) {
		result.scans = parseFiles(data.vfiles.v, editData);
	}
	if (data.has_selfie()) {
		result.selfie = parseFile(data.vselfie, editData);
	}
	if (data.has_plain_data()) {
		switch (data.vplain_data.type()) {
		case mtpc_securePlainPhone: {
			const auto &fields = data.vplain_data.c_securePlainPhone();
			result.data.parsed.fields["value"] = qs(fields.vphone);
		} break;
		case mtpc_securePlainEmail: {
			const auto &fields = data.vplain_data.c_securePlainEmail();
			result.data.parsed.fields["value"] = qs(fields.vemail);
		} break;
		}
	}
	return result;
}

auto FormController::findEditFile(const FullMsgId &fullId) -> EditFile* {
	const auto found = [&](const EditFile &file) {
		return (file.uploadData && file.uploadData->fullId == fullId);
	};
	for (auto &[type, value] : _form.values) {
		for (auto &scan : value.scansInEdit) {
			if (found(scan)) {
				return &scan;
			}
		}
		if (value.selfieInEdit && found(*value.selfieInEdit)) {
			return &*value.selfieInEdit;
		}
	}
	return nullptr;
}

auto FormController::findEditFile(const FileKey &key) -> EditFile* {
	const auto found = [&](const EditFile &file) {
		return (file.fields.dcId == key.dcId && file.fields.id == key.id);
	};
	for (auto &[type, value] : _form.values) {
		for (auto &scan : value.scansInEdit) {
			if (found(scan)) {
				return &scan;
			}
		}
		if (value.selfieInEdit && found(*value.selfieInEdit)) {
			return &*value.selfieInEdit;
		}
	}
	return nullptr;
}

auto FormController::findFile(const FileKey &key)
-> std::pair<Value*, File*> {
	const auto found = [&](const File &file) {
		return (file.dcId == key.dcId) && (file.id == key.id);
	};
	for (auto &[type, value] : _form.values) {
		for (auto &scan : value.scans) {
			if (found(scan)) {
				return { &value, &scan };
			}
		}
		if (value.selfie && found(*value.selfie)) {
			return { &value, &*value.selfie };
		}
	}
	return { nullptr, nullptr };
}

void FormController::formDone(const MTPaccount_AuthorizationForm &result) {
	parseForm(result);
	if (!_passwordRequestId) {
		showForm();
	}
}

void FormController::parseForm(const MTPaccount_AuthorizationForm &result) {
	Expects(result.type() == mtpc_account_authorizationForm);

	const auto &data = result.c_account_authorizationForm();

	App::feedUsers(data.vusers);

	for (const auto &value : data.vvalues.v) {
		auto parsed = parseValue(value);
		const auto type = parsed.type;
		const auto alreadyIt = _form.values.find(type);
		if (alreadyIt != _form.values.end()) {
			LOG(("API Error: Two values for type %1 in authorization form"
				"%1").arg(int(type)));
			continue;
		}
		_form.values.emplace(type, std::move(parsed));
	}
	_form.identitySelfieRequired = data.is_selfie_required();
	if (data.has_privacy_policy_url()) {
		_form.privacyPolicyUrl = qs(data.vprivacy_policy_url);
	}
	for (const auto &required : data.vrequired_types.v) {
		const auto type = ConvertType(required);
		_form.request.push_back(type);
		_form.values.emplace(type, Value(type));
	}
	_bot = App::userLoaded(_request.botId);
}

void FormController::formFail(const RPCError &error) {
	Ui::show(Box<InformBox>(lang(lng_passport_form_error)));
}

void FormController::requestPassword() {
	_passwordRequestId = request(MTPaccount_GetPassword(
	)).done([=](const MTPaccount_Password &result) {
		_passwordRequestId = 0;
		passwordDone(result);
	}).fail([=](const RPCError &error) {
		formFail(error);
	}).send();
}

void FormController::passwordDone(const MTPaccount_Password &result) {
	switch (result.type()) {
	case mtpc_account_noPassword:
		parsePassword(result.c_account_noPassword());
		break;

	case mtpc_account_password:
		parsePassword(result.c_account_password());
		break;
	}
	if (!_formRequestId) {
		showForm();
	}
}

void FormController::showForm() {
	if (!_bot) {
		Ui::show(Box<InformBox>("Could not get authorization bot."));
		return;
	}
	if (!_password.salt.empty()) {
		_view->showAskPassword();
	} else if (!_password.unconfirmedPattern.isEmpty()) {
		_view->showPasswordUnconfirmed();
	} else {
		_view->showNoPassword();
	}
}

void FormController::passwordFail(const RPCError &error) {
	Ui::show(Box<InformBox>("Could not get authorization form."));
}

void FormController::parsePassword(const MTPDaccount_noPassword &result) {
	_password.unconfirmedPattern = qs(result.vemail_unconfirmed_pattern);
	_password.newSalt = bytes::make_vector(result.vnew_salt.v);
	_password.newSecureSalt = bytes::make_vector(result.vnew_secure_salt.v);
	openssl::AddRandomSeed(bytes::make_span(result.vsecure_random.v));
}

void FormController::parsePassword(const MTPDaccount_password &result) {
	_password.hint = qs(result.vhint);
	_password.hasRecovery = mtpIsTrue(result.vhas_recovery);
	_password.salt = bytes::make_vector(result.vcurrent_salt.v);
	_password.unconfirmedPattern = qs(result.vemail_unconfirmed_pattern);
	_password.newSalt = bytes::make_vector(result.vnew_salt.v);
	_password.newSecureSalt = bytes::make_vector(result.vnew_secure_salt.v);
	openssl::AddRandomSeed(bytes::make_span(result.vsecure_random.v));
}

void FormController::cancel() {
	if (!_cancelled) {
		_cancelled = true;
		crl::on_main(this, [=] {
			_controller->clearPassportForm();
		});
	}
}

rpl::lifetime &FormController::lifetime() {
	return _lifetime;
}

FormController::~FormController() = default;

} // namespace Passport